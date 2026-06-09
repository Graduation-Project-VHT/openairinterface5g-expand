/*
 * This module integrates a trained ONNX neural network into OAI's MAC layer
 * as a fifth pluggable scheduler. * State vector layout (77 floats, MUST match Python _build_state()):
 *   [0  – 14]:  CQI normalized,       per UE slot (cqi / 15.0)
 *   [15 – 29]:  Buffer normalized,    per UE slot (bytes / 100000.0)
 *   [30 – 44]:  HOL delay normalized, per UE slot (ms / 500.0)
 *   [45 – 59]:  EWMA throughput,      per UE slot (already normalized)
 *   [60 – 74]:  QCI class encoded,    per UE slot (QCI1=0.0, QCI4=0.5, QCI9=1.0)
 *   [75]:       Jain's Fairness Index (global)
 *   [76]:       Remaining RBs / n_usable_rbs (global)
 *
 * Inactive UE slots (index >= n_active_ues) are zero-padded.
 * Action masking: Q-values for inactive slots are ignored during argmax.
 */

#ifndef SCHED_AI_H
#define SCHED_AI_H

#include "mac.h"
#include "mac_extern.h"

/* =========================================================================
 * Constants — these MUST be kept in sync with Python config.py
 * ========================================================================= */

/* Maximum UEs the ONNX model was trained for. Network input = N_UES_MAX*5+2 */
#define AI_N_UES_MAX          15

/* Total input dimension of the ONNX model: 15*5 + 2 = 77 */
#define AI_STATE_DIM          77

/* Features per UE in the state vector */
#define AI_FEATURES_PER_UE    5

/* Normalization constants — match EnvConfig in config.py */
#define AI_CQI_MAX            15.0f
#define AI_BUFFER_MAX         100000.0f
#define AI_HOL_MAX            500.0f

/* Path to the ONNX model file inside the container.
 * Both the .onnx and .onnx.data files must live in the same directory.
 * Override at compile time with -DAI_ONNX_MODEL_PATH='"..."' if needed. */
#ifndef AI_ONNX_MODEL_PATH
#define AI_ONNX_MODEL_PATH    "./AI_Model/dqn_20MHz_dinal.onnx"
#endif

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * init_ai_scheduler() — Load the ONNX model into memory.
 *
 * Must be called once during eNB MAC initialization, before the first TTI.
 * Logs a fatal error and disables AI mode if the model file cannot be loaded.
 *
 * Called from: the same startup path that calls init_mlwdf_scheduler().
 */
void init_ai_scheduler(void);

/**
 * schedule_ue_spec_ai() — Per-TTI DL scheduling function.
 *
 * Called every 1ms by eNB_dlsch_ulsch_scheduler() when scheduler_mode == AI.
 * Builds the 77-float state vector from OAI MAC structures, runs the ONNX
 * forward pass to obtain 15 UE Q-values, reorders the UE linked list by
 * descending Q-value, then delegates PRB allocation to schedule_dlsch()
 * via the mode trick (same pattern as schedule_ue_spec_mlwdf).
 *
 * Parameters match all other schedule_ue_spec_* functions exactly.
 */
void schedule_ue_spec_ai(module_id_t module_idP,
                         frame_t     frameP,
                         sub_frame_t subframeP,
                         int        *mbsfn_flag);

#endif /* SCHED_AI_H */
