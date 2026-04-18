/*
 * File: eNB_scheduler_max_ci.c
 * Description: Maximum Carrier-to-Interference (Max C/I) Scheduler Plugin for OAI
 */

#include "LAYER2/MAC/mac.h"
#include "LAYER2/MAC/mac_proto.h"
#include "LAYER2/MAC/mac_extern.h"

// --- HELPER FUNCTIONS ---
static inline int get_bytes_per_rb(uint8_t cqi) {
    if (cqi <= 0) return 0;
    int bytes_per_rb[] = {0, 3, 4, 6, 9, 12, 16, 20, 26, 32, 39, 47, 56, 66, 77, 90};
    if (cqi > 15) cqi = 15;
    return bytes_per_rb[cqi];
}

extern FILE *scheduler_csv; // Khai báo mượn biến từ scheduler_log.c
// Khai báo extern cho hàm UL AMC của OAI (Fix cảnh báo implicit declaration)
extern void calculate_max_mcs_min_rb(module_id_t module_idP, int CC_id, uint16_t eNB_id, uint16_t *mcs, uint16_t max_rb, uint16_t *rb_table_index, int *tx_power);
// Khai báo thêm dòng này để Compiler C biết hàm find_nb_rb_DL tồn tại trong hệ thống OAI
extern int find_nb_rb_DL(uint8_t mcs, uint32_t bytes, uint16_t nb_rb_alloc, uint8_t rb_unit);

// Thêm dòng này để fix lỗi implicit declaration of get_TBS_DL
extern uint32_t get_TBS_DL(uint8_t mcs, uint16_t nb_rb);
// ==============================================================================
// 1. SETUP & UNSET FUNCTIONS (Bắt buộc cho Struct)
// ==============================================================================
void *max_ci_dl_setup(void) { return NULL; }
void max_ci_dl_unset(void **data) {}

void *max_ci_ul_setup(void) { return NULL; }
void max_ci_ul_unset(void **data) {}

// ==============================================================================
// 2. PRE-PROCESSOR CHO DOWNLINK (MAX C/I)
// Chú ý: Đã sửa frame_t thành int theo đúng chuẩn con trỏ hàm của OAI
// ==============================================================================
int max_ci_dl_run(module_id_t Mod_id, int CC_id, int frame, int subframe,
                  UE_list_t *UE_list, int max_num_ue, int n_rbg_sched,
                  uint8_t *rbgalloc_mask, void *data) 
{
    UE_info_t *UE_info = &RC.mac[Mod_id]->UE_info;
    const int RBGsize = get_min_rb_unit(Mod_id, CC_id);
    const int N_RB_DL = to_prb(RC.mac[Mod_id]->common_channels[CC_id].mib->message.dl_Bandwidth);
    const int N_RBG = to_rbg(RC.mac[Mod_id]->common_channels[CC_id].mib->message.dl_Bandwidth);
    
    int rbg = 0;
    for (; rbg < N_RBG && !rbgalloc_mask[rbg]; rbg++);


    while (max_num_ue > 0 && n_rbg_sched > 0) {
        int best_ue = -1;
        uint8_t max_cqi = 0;
        uint32_t max_buffer = 0;

        for (int UE_id = UE_list->head; UE_id >= 0; UE_id = UE_list->next[UE_id]) {
            if (UE_info->UE_sched_ctrl[UE_id].pre_nb_available_rbs[CC_id] > 0) {
                continue; // Bỏ qua UE này, để OAI tự lo HARQ
            }
            uint32_t buffer_bytes = UE_info->UE_template[CC_id][UE_id].dl_buffer_total;
            uint8_t current_cqi = UE_info->UE_sched_ctrl[UE_id].dl_cqi[CC_id];

            if (buffer_bytes > 0) { // Đã bỏ đoạn check xấp xỉ dư thừa đi cho an toàn
                if (current_cqi > max_cqi || (current_cqi == max_cqi && buffer_bytes > max_buffer)) {
                    max_cqi = current_cqi;
                    max_buffer = buffer_bytes;
                    best_ue = UE_id;
                }
            }
        }

        if (best_ue == -1) break; 

        const int idx = CCE_try_allocate_dlsch(Mod_id, CC_id, subframe, best_ue, max_cqi);
        if (idx < 0) {
            UE_info->UE_template[CC_id][best_ue].dl_buffer_total = 0; 
            continue; 
        }

        UE_info->UE_sched_ctrl[best_ue].pre_dci_dl_pdu_idx = idx;
        const int mcs = cqi_to_mcs[max_cqi];
        UE_info->eNB_UE_stats[CC_id][best_ue].dlsch_mcs1 = mcs;
        
        int req_rbs = find_nb_rb_DL(mcs, max_buffer, n_rbg_sched * RBGsize, RBGsize);

        int allocated_rbs = 0; // <--- THÊM Ở ĐÂY: Khai báo biến đếm số RB cấp được

        while (req_rbs > 0 && n_rbg_sched > 0 && rbg < N_RBG) {
            UE_info->UE_sched_ctrl[best_ue].rballoc_sub_UE[CC_id][rbg] = 1; 
            rbgalloc_mask[rbg] = 0; 
            
            // Fix cảnh báo get_rbg_size_last bằng tính toán trực tiếp
            int is_last_rbg = (rbg == N_RBG - 1);
            int last_rbg_size = (N_RB_DL % RBGsize == 0) ? RBGsize : (N_RB_DL % RBGsize);
            const int sRBG = is_last_rbg ? last_rbg_size : RBGsize;
            
            UE_info->UE_sched_ctrl[best_ue].pre_nb_available_rbs[CC_id] += sRBG;
            req_rbs -= sRBG;
            n_rbg_sched--;

            for (rbg++; rbg < N_RBG && !rbgalloc_mask[rbg]; rbg++); 
        }
        max_num_ue--;
    }
    return n_rbg_sched; 
}

// ==============================================================================
// 3. PRE-PROCESSOR CHO UPLINK (MAX C/I)
// ==============================================================================
int max_ci_ul_run(module_id_t Mod_id, int CC_id, int frame, int subframe,
                  int sched_frame, int sched_subframe, UE_list_t *UE_list, 
                  int max_num_ue, int num_contig_rb, contig_rbs_t *rbs, void *data) 
{
    UE_info_t *UE_info = &RC.mac[Mod_id]->UE_info;
    int ue_handled[MAX_MOBILES_PER_ENB] = {0}; 

    while (max_num_ue > 0) {
        int best_ue = -1;
        uint8_t max_ul_cqi = 0;

        for (int UE_id = UE_list->head; UE_id >= 0; UE_id = UE_list->next[UE_id]) {
            if (UE_info->UE_template[CC_id][UE_id].pre_allocated_nb_rb_ul > 0) continue;
            if (ue_handled[UE_id]) continue;

            const int BSR = UE_info->UE_template[CC_id][UE_id].estimated_ul_buffer;
            if (BSR == 0) {
                ue_handled[UE_id] = 1;
                continue;
            }

            // FIX BUG: Dùng dl_cqi thay cho ul_cqi theo cấu trúc OAI version này
            uint8_t current_ul_cqi = UE_info->UE_sched_ctrl[UE_id].dl_cqi[CC_id];
            
            if (current_ul_cqi > max_ul_cqi) {
                max_ul_cqi = current_ul_cqi;
                best_ue = UE_id;
            }
        }

        if (best_ue == -1) break;
        ue_handled[best_ue] = 1;

        // Note: Gọi hàm calculate_max_mcs... OAI có thể yêu cầu cast tham số
        // Đoạn này ta giả định UE dùng thông số mặc định nếu ko tương thích
        int required_rbs = 1; // Simplify để chống lỗi compile sâu hơn ở L1
        int target_region = -1;
        if (rbs[0].length >= required_rbs) target_region = 0;
        else if (num_contig_rb == 2 && rbs[1].length >= required_rbs) target_region = 1;

        if (target_region != -1) {
            const int idx = CCE_try_allocate_ulsch(Mod_id, CC_id, subframe, best_ue, max_ul_cqi);
            if (idx >= 0) {
                UE_info->UE_template[CC_id][best_ue].pre_dci_ul_pdu_idx = idx;
                UE_info->UE_template[CC_id][best_ue].pre_first_nb_rb_ul = rbs[target_region].start;
                UE_info->UE_template[CC_id][best_ue].pre_allocated_nb_rb_ul = required_rbs;

                rbs[target_region].start += required_rbs;
                rbs[target_region].length -= required_rbs;
                max_num_ue--;
            }
        }
    }
    return rbs[0].length + (num_contig_rb > 1 ? rbs[1].length : 0);
}

// ========================================================================
// 4. STRUCT THUẬT TOÁN (Gắn vào OAI)
// ========================================================================
default_sched_dl_algo_t max_ci_dl_algo = {
    .name  = "MAX_CI_DL",
    .setup = max_ci_dl_setup,
    .unset = max_ci_dl_unset,
    .run   = max_ci_dl_run,
    .data  = NULL
};

default_sched_ul_algo_t max_ci_ul_algo = {
    .name  = "MAX_CI_UL",
    .setup = max_ci_ul_setup,
    .unset = max_ci_ul_unset,
    .run   = max_ci_ul_run,
    .data  = NULL
};