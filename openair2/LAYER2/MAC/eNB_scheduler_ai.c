/*
 * Architecture:
 *   Each TTI, this module:
 *     1. Counts active UEs and builds a 77-float state vector (zero-padded
 *        for inactive UE slots to support models trained on up to 15 UEs).
 *     2. Runs an ONNX forward pass via the onnxruntime C API to get 15
 *        Q-values (one per UE slot).
 *     3. Applies action masking: only Q-values for slots 0..(n_active-1)
 *        are considered; inactive slots are treated as -infinity.
 *     4. Sorts active UEs by descending Q-value to produce a priority order.
 *     5. Rewrites OAI's UE linked list to reflect that priority order.
 *     6. Uses the "mode trick" (sets scheduler_mode=0 temporarily) to
 *        delegate the actual PRB allocation to schedule_dlsch(), which
 *        allocates RBs greedily from the head of the linked list.
 *
 * State vector layout (MUST match Python stub_env.py _build_state()):
 *   Slot mapping: UE_id i → array slot i (i = 0..MAX_MOBILES_PER_ENB-1)
 *   The first n_active_ues slots are filled with real data.
 *   Slots n_active_ues..14 are zero-padded.
 *
  *   [0  - 14]:  CQI / 15.0          (feature 0 for each UE slot)
  *   [15 - 29]:  buffer / 100000.0   (feature 1)
  *   [30 - 44]:  hol_delay / 500.0   (feature 2)
  *   [45 - 59]:  ewma_tput_norm      (feature 3, already in [0,1])
  *   [60 - 74]:  qci_encoded         (feature 4: QCI1=0.0, QCI4=0.5, QCI9=1.0)
  *   [75]:       Jain's Fairness Index (global)
  *   [76]:       rbs_remaining / n_usable_rbs (global)
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "common/utils/LOG/log.h"
#include "LAYER2/MAC/mac.h"
#include "LAYER2/MAC/mac_extern.h"
#include "LAYER2/MAC/mac_proto.h"
#include "common/ran_context.h"

/* onnxruntime C API */
#include <onnxruntime_c_api.h>

#include "eNB_scheduler_ai.h"

extern RAN_CONTEXT_t RC;

/* Module-level state — loaded once at startup, used every TTI
The global onnxruntime API entry point */

static const OrtApi *g_ort = NULL;

/* ONNX session objects */
static OrtEnv          *g_env     = NULL;
static OrtSessionOptions *g_opts  = NULL;
static OrtSession      *g_session = NULL;

/* Persistent per-UE EWMA throughput tracker.
 * We maintain this in C because OAI does not expose a directly accessible
 * per-UE EWMA throughput field that matches our training definition.
 * Updated every TTI after scheduling. Units: normalized [0, 1]. */
static float g_ewma_tput[MAX_MOBILES_PER_ENB] = {0.0f};

/* Persistent per-UE HOL delay tracker (ms).
 * Incremented for UEs with non-empty buffers, reset when buffer drains. */
static float g_hol_delay_ms[MAX_MOBILES_PER_ENB] = {0.0f};

/* Track whether the scheduler has been initialized */
static int g_initialized = 0;

// Stores tensor name
static char g_input_name[128]  = {0};
static char g_output_name[128] = {0};

/* =========================================================================
 * Helper: fatal ORT error check
 * ========================================================================= */
static int _ort_check(OrtStatus *status, const char *context)
{
    if (status != NULL) {
        const char *msg = g_ort->GetErrorMessage(status);
        LOG_E(MAC, "[AI_SCHED] ONNX Runtime error in %s: %s\n", context, msg);
        g_ort->ReleaseStatus(status);
        AssertFatal(0, "[AI_SCHED] Fatal ONNX error — cannot continue.\n");
        return 1;
    }
    return 0;
}

/* =========================================================================
 * init_ai_scheduler() — Load ONNX model once at startup
 * ========================================================================= */
void init_ai_scheduler(void)
{
    if (g_initialized) {
        LOG_W(MAC, "[AI_SCHED] init_ai_scheduler() called more than once — skipping.\n");
        return;
    }

    LOG_I(MAC, "[AI_SCHED] Initializing DQN scheduler. Model: %s\n",
          AI_ONNX_MODEL_PATH);

    /* Get the global ORT API struct */
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (g_ort == NULL) {
        LOG_E(MAC, "[AI_SCHED] Failed to get ORT API.\n");
        return;
    }

    if (_ort_check(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "sched_ai", &g_env),
                    "CreateEnv") != 0) return;

    if (_ort_check(g_ort->CreateSessionOptions(&g_opts), "CreateSessionOptions") != 0) return;
    if (_ort_check(g_ort->SetIntraOpNumThreads(g_opts, 1), "SetIntraOpNumThreads") != 0) return;
    if (_ort_check(g_ort->SetInterOpNumThreads(g_opts, 1), "SetInterOpNumThreads") != 0) return;

    if (_ort_check(g_ort->CreateSession(g_env, AI_ONNX_MODEL_PATH, g_opts, &g_session),
                    "CreateSession") != 0) return;

    /* Query actual tensor names from the loaded model — never hardcode these */
    OrtAllocator *allocator = NULL;
    if (_ort_check(g_ort->GetAllocatorWithDefaultOptions(&allocator),
                    "GetAllocatorWithDefaultOptions") != 0) return;

    char *raw_name = NULL;
    if (_ort_check(g_ort->SessionGetInputName(g_session, 0, allocator, &raw_name),
                    "SessionGetInputName") != 0) return;
    strncpy(g_input_name, raw_name, sizeof(g_input_name) - 1);
    allocator->Free(allocator, raw_name);
    LOG_I(MAC, "[AI_SCHED] Input tensor name: \"%s\"\n", g_input_name);

    raw_name = NULL;
    if (_ort_check(g_ort->SessionGetOutputName(g_session, 0, allocator, &raw_name),
                    "SessionGetOutputName") != 0) return;
    strncpy(g_output_name, raw_name, sizeof(g_output_name) - 1);
    allocator->Free(allocator, raw_name);
    LOG_I(MAC, "[AI_SCHED] Output tensor name: \"%s\"\n", g_output_name);

    memset(g_ewma_tput,   0, sizeof(g_ewma_tput));
    memset(g_hol_delay_ms, 0, sizeof(g_hol_delay_ms));

    g_initialized = 1;
    LOG_I(MAC, "[AI_SCHED] Model loaded successfully. "
                "State dim: %d, Max UEs: %d\n", AI_STATE_DIM, AI_N_UES_MAX);
}

/* =========================================================================
 * Helper: build_state_vector()
 *
 * Fills a 102-float array from OAI MAC structures for all active UEs.
 * Inactive UE slots are zero-padded.
 *
 * slot_map[i] = UE_id of the i-th active UE (i = 0..n_active-1).
 * This is needed so we can map Q-value index back to OAI UE_id after inference.
 * ========================================================================= */
static void build_state_vector(module_id_t module_idP,
                                float      *state,          /* out: 102 floats */
                                int        *slot_map,       /* out: slot→UE_id */
                                int        *n_active_out,   /* out: active UE count */
                                int         n_usable_rbs,
                                int         rbs_remaining)
{
    eNB_MAC_INST *eNB     = RC.mac[module_idP];
    UE_info_t    *UE_info = &eNB->UE_info;

    /* Zero the entire 102-float vector first — this handles all padding */
    memset(state, 0, AI_STATE_DIM * sizeof(float));

    int slot = 0; /* next available slot index in the state vector */

    for (int UE_id = 0; UE_id < MAX_MOBILES_PER_ENB && slot < AI_N_UES_MAX; UE_id++) {
        if (!UE_info->active[UE_id])
            continue;

        UE_sched_ctrl_t *ctrl = &UE_info->UE_sched_ctrl[UE_id];

        /* --- Feature 0: CQI normalized --- */
        float cqi = (float)ctrl->dl_cqi[0];  /* CC_id = 0 */
        if (cqi < 1.0f) cqi = 1.0f;
        if (cqi > 15.0f) cqi = 15.0f;
        state[slot] = cqi / AI_CQI_MAX;

        /* --- Feature 1: DL buffer occupancy normalized --- */
        int buf_bytes = 0;
        for (int lcid = 0; lcid < MAX_NUM_LCID; lcid++) {
            buf_bytes += UE_info->UE_template[0][UE_id].dl_buffer_info[lcid];
        }
        float buf_norm = (float)buf_bytes / AI_BUFFER_MAX;
        if (buf_norm > 1.0f) buf_norm = 1.0f;
        state[AI_N_UES_MAX + slot] = buf_norm;

        /* --- Feature 2: HOL delay normalized --- */
        float hol_norm = g_hol_delay_ms[UE_id] / AI_HOL_MAX;
        if (hol_norm > 1.0f) hol_norm = 1.0f;
        state[2 * AI_N_UES_MAX + slot] = hol_norm;

        /* --- Feature 3: EWMA throughput (already normalized) --- */
        state[3 * AI_N_UES_MAX + slot] = g_ewma_tput[UE_id];

        /* --- Feature 4: QCI class encoded --- */
        /* Read QCI from the first active logical channel of this UE */
        float qci_encoded = 1.0f; /* default: QCI9 = best-effort = 1.0 */
        for (int lcid = 1; lcid < MAX_NUM_LCID; lcid++) {
            int qci = UE_info->UE_template[0][UE_id].lcgidmap[lcid];
            if (qci == 1) { qci_encoded = 0.0f; break; }  /* URLLC / voice */
            if (qci == 4) { qci_encoded = 0.5f; break; }  /* video */
        }
        state[4 * AI_N_UES_MAX + slot] = qci_encoded;

        /* Record slot→UE_id mapping */
        slot_map[slot] = UE_id;
        slot++;
    }

    *n_active_out = slot;

    /* --- Global feature 0: Jain's Fairness Index --- */
    /* Computed over EWMA throughputs of active UEs */
    float sum_x  = 0.0f;
    float sum_x2 = 0.0f;
    for (int i = 0; i < slot; i++) {
        float x = g_ewma_tput[slot_map[i]];
        sum_x  += x;
        sum_x2 += x * x;
    }
    float jfi = 1.0f;  /* perfect fairness when all zero (startup) */
    if (sum_x2 > 1e-9f) {
        jfi = (sum_x * sum_x) / ((float)slot * sum_x2);
    }
    state[AI_STATE_DIM - 2] = jfi;

    /* --- Global feature 1: remaining RBs this TTI normalized --- */
    float rbs_norm = (float)rbs_remaining / (float)n_usable_rbs;
    if (rbs_norm < 0.0f) rbs_norm = 0.0f;
    if (rbs_norm > 1.0f) rbs_norm = 1.0f;
    state[AI_STATE_DIM - 1] = rbs_norm;
}

/* =========================================================================
 * Helper: run_inference()
 *
 * Feeds the 102-float state through the ONNX model and writes 20 Q-values
 * into q_out[]. Returns 0 on success, -1 on error (falls back to RR).
 * ========================================================================= */
static int run_inference(const float *state, float *q_out)
{
    OrtMemoryInfo *mem_info = NULL;
    OrtValue      *input_tensor  = NULL;
    OrtValue      *output_tensor = NULL;
    int ret = 0;

    _ort_check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                           &mem_info),
               "CreateCpuMemoryInfo");

    /* Input shape: [1, 102] — batch size 1 */
    int64_t input_shape[2] = {1, AI_STATE_DIM};
    _ort_check(g_ort->CreateTensorWithDataAsOrtValue(
                   mem_info,
                   (void *)state,
                   AI_STATE_DIM * sizeof(float),
                   input_shape, 2,
                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                   &input_tensor),
               "CreateTensorWithDataAsOrtValue");

    // const char *input_names[]  = {"input"};
    // const char *output_names[] = {"output"};
    const char *input_names[]  = {g_input_name};
    const char *output_names[] = {g_output_name};

    _ort_check(g_ort->Run(g_session, NULL,
                           input_names,  (const OrtValue *const *)&input_tensor,  1,
                           output_names, 1, &output_tensor),
               "Run");

    /* Extract output data */
    float *out_ptr = NULL;
    _ort_check(g_ort->GetTensorMutableData(output_tensor, (void **)&out_ptr),
               "GetTensorMutableData");

    memcpy(q_out, out_ptr, AI_N_UES_MAX * sizeof(float));

    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);

    return ret;
}

/* =========================================================================
 * Helper: sort_by_q_value()
 *
 * Simple insertion sort on the slot_map array, ordering by descending Q-value.
 * n_active is typically ≤ 20, so O(n²) is fine here.
 * After sorting, slot_map[0] = UE_id with highest Q-value, etc.
 * ========================================================================= */
static void sort_by_q_value(int *slot_map, float *q_values, int n_active)
{
    for (int i = 1; i < n_active; i++) {
        int   key_id = slot_map[i];
        float key_q  = q_values[i];
        int j = i - 1;
        while (j >= 0 && q_values[j] < key_q) {
            slot_map[j + 1] = slot_map[j];
            q_values[j + 1] = q_values[j];  /* NOTE: q_values must be mutable */
            j--;
        }
        slot_map[j + 1] = key_id;
        /* q_values array is local to the caller, cast needed to assign */
        q_values[j + 1] = key_q;
    }
}

/* =========================================================================
 * Helper: reorder_ue_list()
 *
 * Rewrites OAI's UE linked list so that UEs are visited in the order
 * given by sorted_ids[0..n_active-1]. This is the same technique used
 * by eNB_scheduler_mlwdf.c to impose a scheduling priority order before
 * handing off to schedule_dlsch().
 * ========================================================================= */
static void reorder_ue_list(module_id_t module_idP,
                             const int  *sorted_ids,
                             int         n_active)
{
    UE_info_t *UE_info = &RC.mac[module_idP]->UE_info;

    if (n_active == 0) return;

    UE_info->list.head = sorted_ids[0];
    for (int i = 0; i < n_active - 1; i++) {
        UE_info->list.next[sorted_ids[i]] = sorted_ids[i + 1];
    }
    UE_info->list.next[sorted_ids[n_active - 1]] = -1;
}

/* =========================================================================
 * Helper: update_ewma_and_hol()
 *
 * Called at the end of each TTI to update our persistent EWMA throughput
 * and HOL delay trackers. Must be called AFTER schedule_dlsch() returns.
 * ========================================================================= */
static void update_ewma_and_hol(module_id_t module_idP)
{
    eNB_MAC_INST *eNB     = RC.mac[module_idP];
    UE_info_t    *UE_info = &eNB->UE_info;

    /* Maximum bytes deliverable in one TTI at CQI=15 across 92 usable RBs.
     * 100 bytes/RB (CQI=15 from Python stub) × 92 RBs = 9200 bytes. */
    static const float MAX_BYTES_PER_TTI = 9200.0f;

    for (int UE_id = 0; UE_id < MAX_MOBILES_PER_ENB; UE_id++) {
        if (!UE_info->active[UE_id]) {
            /* Reset tracking for detached UEs */
            g_ewma_tput[UE_id]    = 0.0f;
            g_hol_delay_ms[UE_id] = 0.0f;
            continue;
        }

        /* Bytes delivered this TTI: read from eNB_UE_stats */
        // eNB_UE_STATS *ue_stats = &UE_info->eNB_UE_stats[0][UE_id];
        // float bytes_tx = (float)ue_stats->TBS[0]; /* last TBS in bytes */

        /* Estimate bytes delivered from CQI × allocated RBs.
         * This matches the Python training CQI_BYTES_PER_RB table exactly. */
        static const float cqi_bytes_per_rb[16] = {
            0, 2, 4, 6, 10, 15, 21, 26, 34, 43, 49, 59, 70, 81, 92, 100
        };
        UE_sched_ctrl_t *ctrl = &UE_info->UE_sched_ctrl[UE_id];
        uint8_t cqi = ctrl->dl_cqi[0];
        if (cqi > 15) cqi = 15;
        int nb_rb = (int)UE_info->UE_template[0][UE_id].nb_rb[0];
        float bytes_tx = cqi_bytes_per_rb[cqi] * (float)nb_rb;

        /* Update EWMA: α=0.1 matches Python stub */
        float ratio = bytes_tx / MAX_BYTES_PER_TTI;
        if (ratio > 1.0f) ratio = 1.0f;
        g_ewma_tput[UE_id] = 0.9f * g_ewma_tput[UE_id] + 0.1f * ratio;

        /* Update HOL delay */
        int buf_bytes = 0;
        for (int lcid = 0; lcid < MAX_NUM_LCID; lcid++) {
            buf_bytes += UE_info->UE_template[0][UE_id].dl_buffer_info[lcid];
        }
        if (buf_bytes > 0) {
            g_hol_delay_ms[UE_id] += 1.0f;  /* 1 TTI = 1ms passed with data pending */
            if (g_hol_delay_ms[UE_id] > AI_HOL_MAX)
                g_hol_delay_ms[UE_id] = AI_HOL_MAX;
        } else {
            g_hol_delay_ms[UE_id] = 0.0f;  /* buffer drained — reset */
        }
    }
}

/* =========================================================================
 * schedule_ue_spec_ai() — Main per-TTI entry point
 * ========================================================================= */
void schedule_ue_spec_ai(module_id_t module_idP,
                         frame_t     frameP,
                         sub_frame_t subframeP,
                         int        *mbsfn_flag)
{
    /* Safety net: if somehow called before init, fall back to default */
    if (!g_initialized) {
        LOG_I(MAC, "[AI_SCHED] First TTI — running lazy init.\n");
        init_ai_scheduler();
        if (!g_initialized) {
            /* init failed (model file missing, ONNX error, etc.)
             * Fall back permanently — this message will repeat every TTI
             * until the model file is present and the eNB is restarted. */
            LOG_E(MAC, "[AI_SCHED] Init failed. Check model path: %s\n",
                  AI_ONNX_MODEL_PATH);
            schedule_dlsch(module_idP, frameP, subframeP, mbsfn_flag);
            return;
        }
    }

    eNB_MAC_INST *eNB = RC.mac[module_idP];

    /* ------------------------------------------------------------------ */
    /* Step 1: Build state vector                                          */
    /* ------------------------------------------------------------------ */
    float state[AI_STATE_DIM];
    int   slot_map[AI_N_UES_MAX];  /* slot index → OAI UE_id */
    int   n_active = 0;

    /* For the "remaining RBs" global feature we use full usable RBs at TTI
     * start (the value decreases as RBs are allocated, but we snapshot it
     * here before allocation begins, consistent with Python training). */
    int n_usable_rbs = 92;  /* 20 MHz FDD: 100 PRBs − 8 overhead */

    build_state_vector(module_idP, state, slot_map, &n_active,
                       n_usable_rbs, n_usable_rbs);

    /* If no active UEs, nothing to schedule */
    if (n_active == 0) return;

    /* ------------------------------------------------------------------ */
    /* Step 2: Run ONNX inference                                          */
    /* ------------------------------------------------------------------ */
    float q_values[AI_N_UES_MAX];
    memset(q_values, 0, sizeof(q_values));

    if (run_inference(state, q_values) != 0) {
        /* Inference error — fall back to default scheduler this TTI */
        LOG_W(MAC, "[AI_SCHED] Inference failed at %d.%d — falling back.\n",
              frameP, subframeP);
        schedule_dlsch(module_idP, frameP, subframeP, mbsfn_flag);
        update_ewma_and_hol(module_idP);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* Step 3: Action masking — zero out inactive slot Q-values            */
    /* Active slots are 0..n_active-1 in slot_map.                        */
    /* Inactive slots n_active..AI_N_UES_MAX-1 get -FLT_MAX so argmax     */
    /* can never select them.                                              */
    /* ------------------------------------------------------------------ */
    for (int i = n_active; i < AI_N_UES_MAX; i++) {
        q_values[i] = -FLT_MAX;
    }

    /* ------------------------------------------------------------------ */
    /* Step 4: Sort active UEs by descending Q-value                      */
    /* After this, slot_map[0] = highest-priority UE_id.                  */
    /* ------------------------------------------------------------------ */
    sort_by_q_value(slot_map, q_values, n_active);

    /* Log AI decision at debug level (not every TTI to avoid log spam) */
    static int ai_log_counter = 0;
    if ((ai_log_counter++ % 1000) == 0) {
        LOG_I(MAC, "[AI_SCHED] %d.%d | Active UEs: %d | "
                   "Top UE_id: %d (Q=%.3f)\n",
              frameP, subframeP, n_active,
              slot_map[0], q_values[0]);
    }

    /* ------------------------------------------------------------------ */
    /* Step 5: Reorder OAI UE linked list                                 */
    /* ------------------------------------------------------------------ */
    reorder_ue_list(module_idP, slot_map, n_active);

    /* ------------------------------------------------------------------ */
    /* Step 6: THE MODE TRICK                                              */
    /* schedule_dlsch() checks scheduler_mode internally. If it sees       */
    /* SCHED_MODE_AI (=3) it doesn't know what to do. By briefly setting  */
    /* it to SCHED_MODE_DEFAULT (=0) we make it execute the standard PRB  */
    /* allocation path — which now runs on our AI-sorted UE list.         */
    /* This is identical to what eNB_scheduler_mlwdf.c does.              */
    /* ------------------------------------------------------------------ */
    int original_mode = eNB->scheduler_mode;
    eNB->scheduler_mode = SCHED_MODE_DEFAULT;

    schedule_dlsch(module_idP, frameP, subframeP, mbsfn_flag);

    eNB->scheduler_mode = original_mode;  /* restore SCHED_MODE_AI (=3) */

    /* ------------------------------------------------------------------ */
    /* Step 7: Update persistent EWMA and HOL trackers for next TTI       */
    /* ------------------------------------------------------------------ */
    update_ewma_and_hol(module_idP);
}
