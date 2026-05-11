/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef __MAC_EXTERN_H__
#define __MAC_EXTERN_H__


//#include "PHY/defs_common.h"
#include "mac.h"
#include "RRC/LTE/rrc_defs.h"

extern UE_RRC_INST *UE_rrc_inst;
extern UE_MAC_INST *UE_mac_inst;
extern eNB_DLSCH_INFO eNB_dlsch_info[NUMBER_OF_eNB_MAX][MAX_NUM_CCs][MAX_MOBILES_PER_ENB];
extern int NB_UE_INST;

extern const int cqi_to_mcs[16];
extern const uint8_t rb_table[34];
extern rb_id_t mbms_rab_id;


#if defined(PRE_SCD_THREAD)
extern uint16_t pre_nb_rbs_required[2][MAX_NUM_CCs][NUMBER_OF_UE_MAX];
extern uint8_t dlsch_ue_select_tbl_in_use;
extern uint8_t new_dlsch_ue_select_tbl_in_use;
extern bool pre_scd_activeUE[NUMBER_OF_UE_MAX];
extern eNB_UE_STATS pre_scd_eNB_UE_stats[MAX_NUM_CCs][NUMBER_OF_UE_MAX];
#endif

extern mac_rlc_am_muilist_t rlc_am_mui;
extern SCHEDULER_MODES global_scheduler_mode;
void lte_dump_mac_stats(eNB_MAC_INST *mac, FILE *fd);

#include "common/ran_context.h"
extern RAN_CONTEXT_t RC;
extern rb_id_t mbms_rab_id;

static const uint32_t BSR_TABLE[BSR_TABLE_SIZE] = {0,     10,    12,    14,    17,    19,    22,    26,    31,    36,    42,    49,    57,     67,     78,     91,
                                                   105,   125,   146,   171,   200,   234,   274,   321,   376,   440,   515,   603,   706,    826,    967,    1132,
                                                   1326,  1552,  1817,  2127,  2490,  2915,  3413,  3995,  4677,  5467,  6411,  7505,  8787,   10287,  12043,  14099,
                                                   16507, 19325, 22624, 26487, 31009, 36304, 42502, 49759, 58255, 68201, 79846, 93479, 109439, 128125, 150000, 300000};

// Max C/I
extern int max_ci_dl_run(module_id_t Mod_id, int CC_id, int frame, int subframe, UE_list_t *UE_list, int max_num_ue, int n_rbg_sched, uint8_t *rbgalloc_mask, void *data);

extern int max_ci_ul_run(module_id_t Mod_id, int CC_id, int frame, int subframe, int sched_frame, int sched_subframe, UE_list_t *UE_list, int max_num_ue, int num_contig_rb, contig_rbs_t *rbs, void *data);

extern void init_mac_scheduler_plugins(module_id_t module_idP);
// Xuất (Expose) cấu trúc thuật toán Max C/I để các file khác lấy được
extern default_sched_dl_algo_t max_ci_dl_algo;
extern default_sched_ul_algo_t max_ci_ul_algo;

#endif //DEF_H

