/*
 * File: eNB_scheduler_max_ci.c
 * Description: Maximum Carrier-to-Interference (Max C/I) Scheduler Module
 * Architecture: OpenAirInterface (OAI) compliant (RLC <-> MAC <-> PHY/nFAPI)
 * Author: Duy & Capstone Team
 */

#include "LAYER2/MAC/mac.h"
#include "LAYER2/MAC/mac_proto.h"
#include "LAYER2/MAC/mac_extern.h"
#include "PHY/LTE_TRANSPORT/transport_common_proto.h"
#include "nfapi_interface.h"


// 1. Hàm khởi tạo (Setup) và Hủy (Unset) - Bắt buộc phải có theo Interface
void *max_ci_dl_setup(void) {
    // Nếu thuật toán của bạn cần cấp phát bộ nhớ (malloc), làm ở đây.
    // Max C/I không cần lưu trạng thái (stateless) nên trả về NULL.
    return NULL; 
}

void max_ci_dl_unset(void **data) {
    // Giải phóng bộ nhớ nếu có
}

void *max_ci_ul_setup(void) { return NULL; }
void max_ci_ul_unset(void **data) { }

// ==============================================================================
// BƯỚC 1: PRE-PROCESSOR CHO DOWNLINK (THUẬT TOÁN LÕI - OFDMA)
// ==============================================================================
int max_ci_dl_run(module_id_t Mod_id, int CC_id, frame_t frame, sub_frame_t subframe,
                  UE_list_t *UE_list, int max_num_ue, int n_rbg_sched,
                  uint8_t *rbgalloc_mask, void *data) 
{
    UE_info_t *UE_info = &RC.mac[Mod_id]->UE_info;
    const int RBGsize = get_min_rb_unit(Mod_id, CC_id);
    const int N_RBG = to_rbg(RC.mac[Mod_id]->common_channels[CC_id].mib->message.dl_Bandwidth);
    
    // Bỏ qua các RBG đã bị khóa bởi hệ thống (Ví dụ: Broadcast/Paging)
    int rbg = 0;
    for (; !rbgalloc_mask[rbg]; rbg++);

    // Vòng lặp Greedy: Dồn toàn bộ tài nguyên cho UE có sóng tốt nhất
    while (max_num_ue > 0 && n_rbg_sched > 0) {
        int best_ue = -1;
        uint8_t max_cqi = 0;
        uint32_t max_buffer = 0;

        // Quét tìm UE có CQI cao nhất
        for (int UE_id = UE_list->head; UE_id >= 0; UE_id = UE_list->next[UE_id]) {
            uint32_t buffer_bytes = UE_info->UE_template[CC_id][UE_id].dl_buffer_total;
            uint8_t current_cqi = UE_info->UE_sched_ctrl[UE_id].dl_cqi[CC_id];

            if (buffer_bytes > (UE_info->UE_sched_ctrl[UE_id].pre_nb_available_rbs[CC_id] * get_bytes_per_rb(current_cqi))) {
                if (current_cqi > max_cqi || (current_cqi == max_cqi && buffer_bytes > max_buffer)) {
                    max_cqi = current_cqi;
                    max_buffer = buffer_bytes;
                    best_ue = UE_id;
                }
            }
        }

        if (best_ue == -1) break; // Hết UE cần Data

        // Ràng buộc Hệ thống: Kiểm tra Control Channel Element (CCE)
        const int idx = CCE_try_allocate_dlsch(Mod_id, CC_id, subframe, best_ue, max_cqi);
        if (idx < 0) {
            UE_info->UE_template[CC_id][best_ue].dl_buffer_total = 0; // Hết CCE, bỏ qua UE này
            continue; 
        }

        // Cập nhật thông số cho UE
        UE_info->UE_sched_ctrl[best_ue].pre_dci_dl_pdu_idx = idx;
        const int mcs = cqi_to_mcs[max_cqi];
        UE_info->eNB_UE_stats[CC_id][best_ue].dlsch_mcs1 = mcs;
        
        int req_rbs = find_nb_rb_DL(mcs, max_buffer, n_rbg_sched * RBGsize, RBGsize);

        // Khóa RBG cho UE chiến thắng
        while (req_rbs > 0 && n_rbg_sched > 0) {
            UE_info->UE_sched_ctrl[best_ue].rballoc_sub_UE[CC_id][rbg] = 1; 
            rbgalloc_mask[rbg] = 0; // Đánh dấu đã dùng
            
            const int sRBG = (rbg == N_RBG - 1) ? get_rbg_size_last(Mod_id, CC_id) : RBGsize;
            UE_info->UE_sched_ctrl[best_ue].pre_nb_available_rbs[CC_id] += sRBG;
            req_rbs -= sRBG;
            n_rbg_sched--;

            for (rbg++; n_rbg_sched > 0 && !rbgalloc_mask[rbg]; rbg++); // Nhảy đến RBG rảnh tiếp theo
        }
        max_num_ue--;
    }
    return n_rbg_sched; 
}

// ==============================================================================
// BƯỚC 2: PRE-PROCESSOR CHO UPLINK (THUẬT TOÁN LÕI - SC-FDMA CONTIGUOUS)
// ==============================================================================
int max_ci_ul_run(module_id_t Mod_id, int CC_id, frame_t frame, sub_frame_t subframe,
                  UE_list_t *UE_list, int max_num_ue, int num_contig_rb, 
                  contig_rbs_t *rbs, void *data) 
{
    UE_info_t *UE_info = &RC.mac[Mod_id]->UE_info;
    int ue_handled[MAX_MOBILES_PER_ENB] = {0}; // Mảng đánh dấu UE đã xử lý

    while (max_num_ue > 0) {
        int best_ue = -1;
        uint8_t max_ul_cqi = 0;

        // Quét tìm UE có sóng UL (SRS) tốt nhất
        for (int UE_id = UE_list->head; UE_id >= 0; UE_id = UE_list->next[UE_id]) {
            if (ue_handled[UE_id]) continue;

            const int BSR = UE_info->UE_template[CC_id][UE_id].estimated_ul_buffer;
            if (BSR == 0) {
                ue_handled[UE_id] = 1;
                continue;
            }

            uint8_t current_ul_cqi = UE_info->UE_sched_ctrl[UE_id].ul_cqi[CC_id];
            if (current_ul_cqi > max_ul_cqi) {
                max_ul_cqi = current_ul_cqi;
                best_ue = UE_id;
            }
        }

        if (best_ue == -1) break;
        ue_handled[best_ue] = 1;

        // Tính MCS và RBs dựa trên Power Headroom và BSR
        int selected_mcs, rb_table_index, tx_power;
        int max_rb_available = (num_contig_rb > 1) ? max(rbs[0].length, rbs[1].length) : rbs[0].length;
        
        calculate_max_mcs_min_rb(Mod_id, CC_id, UE_info->UE_template[CC_id][best_ue].estimated_ul_buffer, 
                                 UE_info->UE_template[CC_id][best_ue].phr_info, 20, 
                                 &selected_mcs, max_rb_available, &rb_table_index, &tx_power);
        
        int required_rbs = rb_table[rb_table_index];

        // Ràng buộc SC-FDMA: Tìm Block liên tục
        int target_region = -1;
        if (rbs[0].length >= required_rbs) target_region = 0;
        else if (num_contig_rb == 2 && rbs[1].length >= required_rbs) target_region = 1;

        if (target_region != -1) {
            // Ràng buộc CCE cho Uplink Grant (DCI Format 0)
            const int idx = CCE_try_allocate_ulsch(Mod_id, CC_id, subframe, best_ue, max_ul_cqi);
            if (idx >= 0) {
                UE_info->UE_template[CC_id][best_ue].pre_dci_ul_pdu_idx = idx;
                UE_info->UE_template[CC_id][best_ue].pre_assigned_mcs_ul = selected_mcs;
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
// 3. ĐÓNG GÓI THÀNH STRUCT THUẬT TOÁN (Chuẩn OAI Plugin)
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

// ==============================================================================
// BƯỚC 4: EXECUTION DOWNLINK (GIAO TIẾP END-TO-END RLC -> MAC -> nFAPI)
// ==============================================================================
void schedule_dlsch_max_ci_execution(module_id_t module_idP, frame_t frameP, sub_frame_t subframeP) 
{
    eNB_MAC_INST *eNB = RC.mac[module_idP];
    UE_info_t *UE_info = &eNB->UE_info;
    int CC_id = 0; 

    // [A] INPUT TỪ RLC: Cập nhật Buffer Status
    for (int UE_id = UE_info->list.head; UE_id >= 0; UE_id = UE_info->list.next[UE_id]) {
        mac_rlc_status_resp_t rlc_status = mac_rlc_status_ind(
            module_idP, UE_info->UE_template[CC_id][UE_id].rnti, module_idP, frameP, subframeP, 
            ENB_FLAG_YES, MBMS_FLAG_NO, DTCH, 0, 0
        );
        UE_info->UE_template[CC_id][UE_id].dl_buffer_total = rlc_status.bytes_in_buffer;
        UE_info->UE_template[CC_id][UE_id].dl_hol_delay = rlc_status.head_sdu_creation_time;
    }

    //[B] CHẠY THUẬT TOÁN PRE-PROCESSOR
    uint8_t rbgalloc_mask[100]; 
    memset(rbgalloc_mask, 1, sizeof(rbgalloc_mask)); 
    max_ci_dl_run(module_idP, CC_id, frameP, subframeP, &UE_info->list, MAX_MOBILES_PER_ENB, 
                  to_rbg(eNB->common_channels[CC_id].mib->message.dl_Bandwidth), rbgalloc_mask, NULL);

    // [C] OUTPUT XUỐNG PHY QUA nFAPI
    nfapi_dl_config_request_body_t *dl_req = &eNB->DL_req[CC_id].dl_config_request_body;

    for (int UE_id = UE_info->list.head; UE_id >= 0; UE_id = UE_info->list.next[UE_id]) {
        int allocated_rbs = UE_info->UE_sched_ctrl[UE_id].pre_nb_available_rbs[CC_id];
        if (allocated_rbs == 0) continue; 

        rnti_t rnti = UE_info->UE_template[CC_id][UE_id].rnti;
        int mcs = UE_info->eNB_UE_stats[CC_id][UE_id].dlsch_mcs1;
        int TBS = get_TBS_DL(mcs, allocated_rbs);

        // Yêu cầu RLC chuyển Data xuống MAC
        unsigned char dlsch_buffer[MAX_DLSCH_PAYLOAD_BYTES];
        mac_rlc_data_req(module_idP, rnti, module_idP, frameP, ENB_FLAG_YES, MBMS_FLAG_NO, 
                         DTCH, TBS, (char*)&dlsch_buffer[0], 0, 0);

        // Ghi bản tin DCI (nFAPI)
        int dci_idx = UE_info->UE_sched_ctrl[UE_id].pre_dci_dl_pdu_idx;
        nfapi_dl_config_request_pdu_t *dl_config_pdu = &dl_req->dl_config_pdu_list[dci_idx];
        
        dl_config_pdu->pdu_type = NFAPI_DL_CONFIG_DCI_DL_PDU_TYPE;
        dl_config_pdu->dci_dl_pdu.dci_dl_pdu_rel8.dci_format = NFAPI_DL_DCI_FORMAT_1;
        dl_config_pdu->dci_dl_pdu.dci_dl_pdu_rel8.rnti = rnti;
        dl_config_pdu->dci_dl_pdu.dci_dl_pdu_rel8.mcs_1 = mcs;
        
        // Ghi bản tin cấu hình DLSCH (nFAPI)
        fill_nfapi_dlsch_config(&dl_req->dl_config_pdu_list[dci_idx + 1], TBS, eNB->pdu_index[CC_id], rnti, mcs, ...);
        dl_req->number_pdu += 2; 
    }
}

// ==============================================================================
// BƯỚC 4: EXECUTION UPLINK (GỬI LỆNH DCI0 XUỐNG PHY nFAPI)
// ==============================================================================
void schedule_ulsch_max_ci_execution(module_id_t module_idP, frame_t frameP, sub_frame_t subframeP) 
{
    eNB_MAC_INST *eNB = RC.mac[module_idP];
    UE_info_t *UE_info = &eNB->UE_info;
    int CC_id = 0;

    // [A] KHỞI TẠO KHÔNG GIAN SC-FDMA TỪ PHY
    contig_rbs_t rbs[2];
    int num_contig_rb = get_ul_contiguous_rbs(module_idP, CC_id, rbs);

    //[B] CHẠY THUẬT TOÁN PRE-PROCESSOR UPLINK
    max_ci_ul_run(module_idP, CC_id, frameP, subframeP, &UE_info->list, 
                  MAX_MOBILES_PER_ENB, num_contig_rb, rbs, NULL);

    // [C] OUTPUT XUỐNG PHY (GỬI UPLINK GRANT QUA DCI FORMAT 0)
    nfapi_hi_dci0_request_body_t *hi_dci0_req = &eNB->HI_DCI0_req[CC_id][subframeP].hi_dci0_request_body;

    for (int UE_id = UE_info->list.head; UE_id >= 0; UE_id = UE_info->list.next[UE_id]) {
        UE_TEMPLATE *UE_template = &UE_info->UE_template[CC_id][UE_id];
        if (UE_template->pre_allocated_nb_rb_ul == 0) continue;

        int dci_idx = UE_template->pre_dci_ul_pdu_idx;
        nfapi_hi_dci0_request_pdu_t *hi_dci0_pdu = &hi_dci0_req->hi_dci0_pdu_list[dci_idx];

        // Lấp đầy bản tin nFAPI DCI Format 0 (Báo UE biết vị trí RB để phát sóng lên)
        hi_dci0_pdu->pdu_type = NFAPI_HI_DCI0_DCI_PDU_TYPE;
        hi_dci0_pdu->dci_pdu.dci_pdu_rel8.dci_format = NFAPI_UL_DCI_FORMAT_0;
        hi_dci0_pdu->dci_pdu.dci_pdu_rel8.rnti = UE_template->rnti;
        hi_dci0_pdu->dci_pdu.dci_pdu_rel8.mcs_1 = UE_template->pre_assigned_mcs_ul;
        hi_dci0_pdu->dci_pdu.dci_pdu_rel8.resource_block_start = UE_template->pre_first_nb_rb_ul;
        hi_dci0_pdu->dci_pdu.dci_pdu_rel8.number_of_resource_block = UE_template->pre_allocated_nb_rb_ul;
        
        hi_dci0_req->number_of_dci++;
        
        // Trừ BSR giả định
        UE_template->scheduled_ul_bytes += rb_table[UE_template->pre_allocated_rb_table_index_ul];
    }
}