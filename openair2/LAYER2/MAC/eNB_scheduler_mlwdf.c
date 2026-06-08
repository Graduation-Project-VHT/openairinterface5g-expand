#include <stdio.h>
#include <stdlib.h>
#include <time.h> // Added for random seed generation
#include "eNB_scheduler_mlwdf.h"
#include "LAYER2/MAC/mac.h"
#include "LAYER2/MAC/mac_proto.h"
#include "LAYER2/MAC/mac_extern.h"
#include "common/utils/LOG/log.h"
#include "PHY/LTE_TRANSPORT/transport_common_proto.h"
#include "common/ran_context.h"

extern RAN_CONTEXT_t RC;

// Declare the original OAI scheduling function to be called in Phase 3
extern void schedule_dlsch(module_id_t module_idP, frame_t frameP, sub_frame_t subframeP, int *mbsfn_flag);

// ========================================================================
// GLOBAL VARIABLES (Log bridge & Fixed Profile Storage for UEs)
// ========================================================================
int g_mlwdf_delay[MAX_MOBILES_PER_ENB] = {0};
float g_mlwdf_thr[MAX_MOBILES_PER_ENB] = {0.0};
float g_mlwdf_score[MAX_MOBILES_PER_ENB] = {0.0};

// Arrays storing the assigned profiles for each UE upon eNB startup
float g_ue_qos_alpha[MAX_MOBILES_PER_ENB] = {0.0};

mlwdf_ue_stats_t mlwdf_stats[MAX_MOBILES_PER_ENB];

// ========================================================================
// INITIALIZATION FUNCTION: [TEST SCENARIO] Random QoS (5 Levels) & Random CQI
// ========================================================================
void init_mlwdf_scheduler(void) {
    LOG_I(MAC, "\n======================================================\n");
    LOG_I(MAC, "[MAC-MLWDF] INITIALIZING TEST SCENARIO: 5 RANDOM QoS & RANDOM CQI\n");
    LOG_I(MAC, "======================================================\n");

    // Seed the random number generator
    srand(time(NULL));

    // [NEW]: Define 5 possible QoS weights representing different service classes
    float possible_qos[5] = {5.0, 3.0, 1.2, 0.5, 0.1};

    for (int i = 0; i < MAX_MOBILES_PER_ENB; i++) {
        mlwdf_stats[i].rnti = 0;
        mlwdf_stats[i].avg_throughput_kbps = 1000.0;
        mlwdf_stats[i].hol_delay_ms = 0;

        g_mlwdf_delay[i] = 0;
        g_mlwdf_thr[i] = 0.0;
        g_mlwdf_score[i] = 0.0;

        // [NEW]: Randomly assign one of the 5 QoS Levels
        int random_qos_index = rand() % 5;
        g_ue_qos_alpha[i] = possible_qos[random_qos_index];

        // Randomly assign Radio Condition Profile (0: Center, 1: Mid, 2: Edge)
        // We don't need this anymore since we are using the CQI value from the physical layer
        // g_ue_cqi_profile[i] = rand() % 3;

        // Print initialization logs to confirm random assignment
        LOG_I(MAC, "[MLWDF-INIT] Slot UE_ID %d | Random QoS Alpha: %.1f | Random CQI Profile: %d\n",
               i, g_ue_qos_alpha[i], g_ue_cqi_profile[i]);
    }
    LOG_I(MAC, "======================================================\n\n");
}

// ========================================================================
// Simulate Distance & Attenuation
// ========================================================================
void generate_dynamic_cqi(module_id_t module_idP) {
    // Physical layer uses optimal transmission to prevent 100% BLER.
    // Distance attenuation is simulated via Virtual Rate in Phase 1.
    return;
}

// ========================================================================
// MAIN FUNCTION: QoS Aware M-LWDF Scheduling
// ========================================================================
void schedule_ue_spec_mlwdf(module_id_t module_idP, frame_t frameP, sub_frame_t subframeP, int *mbsfn_flag) {

    // ===================================================================
    // CHỐT CHẶN 1: BẢO VỆ CON TRỎ MAC (Chống Race Condition từ luồng PHY)
    // ===================================================================
    if (RC.mac == NULL || RC.mac[module_idP] == NULL) {
        return; // Nếu bộ nhớ Tầng MAC chưa sẵn sàng, bỏ qua ngay TTI này!
    }

    eNB_MAC_INST *eNB = RC.mac[module_idP];

    // ===================================================================
    // CHỐT CHẶN WARM-UP (LÁ CHẮN THỜI GIAN)
    // Né 100 frame đầu tiên (1 giây) cực kỳ bất ổn của hệ thống OAI.
    // ===================================================================
    if (frameP < 100) {
        int orig_mode = eNB->scheduler_mode;
        eNB->scheduler_mode = 0; // Tạm ép về Default (0) để khởi động an toàn
        schedule_dlsch(module_idP, frameP, subframeP, mbsfn_flag);
        eNB->scheduler_mode = orig_mode; // Trả lại chế độ M-LWDF
        return;
    }

    int CC_id = 0;
    static int is_mlwdf_initialized = 0;
    if (is_mlwdf_initialized == 0) {
        init_mlwdf_scheduler();
        is_mlwdf_initialized = 1;
    }

    UE_info_t *UE_info = &eNB->UE_info;
    int num_active_ues = 0;
    int ue_id;
    mlwdf_ue_stats_t current_sched_list[MAX_MOBILES_PER_ENB];
    uint32_t current_time_ms = (frameP * 10) + subframeP;

    if (frameP > 1000) {
        generate_dynamic_cqi(module_idP);
    }

    // ========================================================================
    // PHASE 1: CALCULATE METRICS FOR EACH ACTIVE UE
    // ========================================================================
    for (ue_id = 0; ue_id < MAX_MOBILES_PER_ENB; ue_id++) {

        if (UE_info->active[ue_id] == 1) {
            rnti_t rnti = UE_RNTI(module_idP, ue_id);
            if (rnti == 0 || rnti == NOT_A_RNTI) continue;

            UE_sched_ctrl_t *ue_sched_ctrl = &UE_info->UE_sched_ctrl[ue_id];

            // 1. Get real rate for actual physical data transmission
            uint8_t cqi = ue_sched_ctrl->dl_cqi[CC_id];

            // ===================================================================
            // CHỐT CHẶN 2: BẢO VỆ MẢNG CQI (Chống rác bộ nhớ làm văng index)
            // ===================================================================
            if (cqi == 0 || cqi > 15) {
                cqi = 5; // Ép về giá trị an toàn nếu biến cqi chứa rác
            }

            uint8_t mcs = cqi_to_mcs[cqi];
            uint32_t real_tbs = get_TBS_DL(mcs, 1);

            // =========================================================
            // APPLY VIRTUAL RATE BASED ON RANDOMIZED PROFILE
            // =========================================================
            /* ── Derive cqi_profile from actual measured CQI ──────────────────
             * Updated every TTI so the CSV label always reflects the real
             * channel condition, not a stale random assignment from init.
             * Thresholds follow 3GPP CQI modulation boundaries:
             *   12–15 → 64QAM  → profile 0 (Center)
             *    7–11 → 16QAM  → profile 1 (Mid)
             *    0–6  → QPSK   → profile 2 (Edge)
             * ─────────────────────────────────────────────────────────────── */
            int profile;
            if      (cqi >= 12) profile = 0;  // Center
            else if (cqi >= 7)  profile = 1;  // Mid
            else                profile = 2;  // Edge

            // Virtual rate multiplier now reflects actual channel quality:
            float virtual_rate = real_tbs * 8.0;
            if (profile == 0) {
                virtual_rate *= 1.0;   // Center — full rate
            } else if (profile == 1) {
                virtual_rate *= 0.5;   // Mid    — half rate
            } else {
                virtual_rate *= 0.1;   // Edge   — 10% rate
            }

            // float virtual_rate = real_tbs * 8.0;
            // int profile = g_ue_cqi_profile[ue_id];

            // if (profile == 0) {
            //     virtual_rate *= 1.0;  // Cell Center
            // } else if (profile == 1) {
            //     virtual_rate *= 0.5;  // Cell Mid
            // } else if (profile == 2) {
            //     virtual_rate *= 0.1;  // Cell Edge
            // }

            current_sched_list[num_active_ues].inst_rate_kbps = virtual_rate;

            // 2. Get average throughput (EMA)
            current_sched_list[num_active_ues].avg_throughput_kbps = mlwdf_stats[ue_id].avg_throughput_kbps;
            float R_avg = current_sched_list[num_active_ues].avg_throughput_kbps;
            if (R_avg <= 100.0) R_avg = 100.0;

            // 3. Calculate HoL (Head-of-Line) Delay
            mac_rlc_status_resp_t rlc_status = mac_rlc_status_ind(
                module_idP, rnti, module_idP, frameP, subframeP,
                ENB_FLAG_YES, MBMS_FLAG_NO, 3, 0, 0
            );

            int32_t delay_ms = 0;
            if (rlc_status.bytes_in_buffer > 0) {
                uint32_t creation_time = rlc_status.head_sdu_creation_time;
                if (creation_time > 0) {
                    if (current_time_ms >= creation_time) delay_ms = current_time_ms - creation_time;
                    else delay_ms = (current_time_ms + 10240) - creation_time;
                } else {
                    delay_ms = (rlc_status.bytes_in_buffer * 8) / R_avg;
                }
            }
            if (delay_ms > 10000) delay_ms = 10000;
            if (delay_ms <= 0) delay_ms = 1;
            current_sched_list[num_active_ues].hol_delay_ms = delay_ms;

            // 4. Calculate M-LWDF score using Randomized QoS
            current_sched_list[num_active_ues].qos_weight_alpha = g_ue_qos_alpha[ue_id];

            current_sched_list[num_active_ues].mlwdf_metric =
                current_sched_list[num_active_ues].qos_weight_alpha * ((float)current_sched_list[num_active_ues].hol_delay_ms / R_avg) * current_sched_list[num_active_ues].inst_rate_kbps;

            // Transfer values to global log arrays
            g_mlwdf_delay[ue_id] = current_sched_list[num_active_ues].hol_delay_ms;
            g_mlwdf_thr[ue_id]  = current_sched_list[num_active_ues].avg_throughput_kbps;
            g_mlwdf_score[ue_id] = current_sched_list[num_active_ues].mlwdf_metric;

            current_sched_list[num_active_ues].rnti = rnti;
            current_sched_list[num_active_ues].ue_id = ue_id;
            num_active_ues++;
        }
    }

    // ========================================================================
    // PHASE 2: SORT UEs BY M-LWDF PRIORITY (Descending)
    // ========================================================================
    for (int i = 0; i < num_active_ues - 1; i++) {
        for (int j = 0; j < num_active_ues - i - 1; j++) {
            if (current_sched_list[j].mlwdf_metric < current_sched_list[j+1].mlwdf_metric) {
                mlwdf_ue_stats_t temp = current_sched_list[j];
                current_sched_list[j] = current_sched_list[j+1];
                current_sched_list[j+1] = temp;
            }
        }
    }

    // ========================================================================
    // PHASE 3: UPDATE LIST & STANDARD M-LWDF EMA
    // ========================================================================
    if (num_active_ues > 0) {
        // 1. Swap OAI linked list pointers
        UE_info->list.head = current_sched_list[0].ue_id;
        for (int i = 0; i < num_active_ues - 1; i++) {
            UE_info->list.next[current_sched_list[i].ue_id] = current_sched_list[i+1].ue_id;
        }
        UE_info->list.next[current_sched_list[num_active_ues - 1].ue_id] = -1;

        // 2. UPDATE EMA: CORE FIX FOR STARVATION
        for (int i = 0; i < num_active_ues; i++) {
            int id = current_sched_list[i].ue_id;

            // Only the winning UE (Top 1) gets the throughput added.
            float assumed_tx_kbps = 0.0;
            if (i == 0) {
                assumed_tx_kbps = current_sched_list[i].inst_rate_kbps;
            }

            mlwdf_stats[id].avg_throughput_kbps =
                (0.99 * mlwdf_stats[id].avg_throughput_kbps) + (0.01 * assumed_tx_kbps);

            // Prevent Divide-by-zero error
            if (mlwdf_stats[id].avg_throughput_kbps < 10.0) {
                mlwdf_stats[id].avg_throughput_kbps = 10.0;
            }
        }
    }

    // ===================================================================
    // CÚ LỪA THẾ KỶ (THE MODE TRICK)
    // Đánh lừa bộ máy OAI rằng nó đang chạy Default để nó chịu cấp phát PRB
    // ===================================================================
    int original_mode = eNB->scheduler_mode;
    eNB->scheduler_mode = 0;

    // 3. Call the original OAI scheduler function
    schedule_dlsch(module_idP, frameP, subframeP, mbsfn_flag);

    eNB->scheduler_mode = original_mode; // Lấy lại thân phận MLWDF (2)
}
