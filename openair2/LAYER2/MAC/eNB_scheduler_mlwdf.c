#include "eNB_scheduler_mlwdf.h"
#include "LAYER2/MAC/mac.h"
#include "LAYER2/MAC/mac_proto.h"
#include "LAYER2/MAC/mac_extern.h"
#include "common/utils/LOG/log.h"
#include "PHY/LTE_TRANSPORT/transport_common_proto.h"
#include "common/ran_context.h"

extern RAN_CONTEXT_t RC;

// Khai báo hàm lập lịch gốc của OAI để gọi ở Giai đoạn 3
extern void schedule_ue_spec(module_id_t module_idP, int CC_id, frame_t frameP, sub_frame_t subframeP);

int g_mlwdf_delay[MAX_MOBILES_PER_ENB] = {0};
float g_mlwdf_thr[MAX_MOBILES_PER_ENB] = {0.0};
float g_mlwdf_score[MAX_MOBILES_PER_ENB] = {0.0};

// Mảng tĩnh lưu trữ trạng thái của các UE để tính trung bình qua các Subframe
mlwdf_ue_stats_t mlwdf_stats[MAX_MOBILES_PER_ENB];

// Hàm khởi tạo
void init_mlwdf_scheduler(void) {
    for (int i = 0; i < MAX_MOBILES_PER_ENB; i++) {
        mlwdf_stats[i].rnti = 0;
        mlwdf_stats[i].avg_throughput_kbps = 1.0; // Tránh lỗi chia cho 0
        mlwdf_stats[i].hol_delay_ms = 0;

        g_mlwdf_delay[i] = 0;
        g_mlwdf_thr[i] = 0.0;
        g_mlwdf_score[i] = 0.0;
    }
}

// Hàm Lập lịch QoS Aware M-LWDF
void schedule_ue_spec_mlwdf(module_id_t module_idP, int CC_id, frame_t frameP, sub_frame_t subframeP) {
    eNB_MAC_INST *eNB = RC.mac[module_idP];
    UE_info_t *UE_info = &eNB->UE_info;

    int num_active_ues = 0;
    int ue_id;

    // Mảng tạm để lưu trữ điểm số trong Subframe hiện tại
    mlwdf_ue_stats_t current_sched_list[MAX_MOBILES_PER_ENB];

    // Có thể comment dòng in này lại cho đỡ rác Terminal vì giờ ta đã có CSV
    // printf("[MAC-MLWDF] --- Bắt đầu lập lịch M-LWDF | Frame: %d, Subframe: %d, CC_id: %d ---\n", frameP, subframeP, CC_id);

    // Lấy thời gian hiện tại của hệ thống (Tính bằng mili-giây: 1 Frame = 10ms, 1 Subframe = 1ms)
    uint32_t current_time_ms = (frameP * 10) + subframeP;

    // ========================================================================
    // GIAI ĐOẠN 1: TÍNH TOÁN METRIC CHO TỪNG UE ĐANG HOẠT ĐỘNG
    // ========================================================================
    for (ue_id = 0; ue_id < MAX_MOBILES_PER_ENB; ue_id++) {

        if (UE_info->active[ue_id] == 1) {

            rnti_t rnti = UE_RNTI(module_idP, ue_id);

            // Bỏ qua nếu RNTI không hợp lệ (mặc dù slot đang active)
            if (rnti == 0 || rnti == NOT_A_RNTI) continue;

            UE_sched_ctrl_t *ue_sched_ctrl = &UE_info->UE_sched_ctrl[ue_id];

            // 1. Tính tốc độ tức thời r_i(t)
            uint8_t cqi = ue_sched_ctrl->dl_cqi[CC_id];
            if (cqi == 0) cqi = 5; // Fallback an toàn nếu CQI chưa kịp báo cáo
            uint8_t mcs = cqi_to_mcs[cqi];
            uint32_t estimated_tbs = get_TBS_DL(mcs, 1);
            current_sched_list[num_active_ues].inst_rate_kbps = estimated_tbs * 8.0;

            // 2. Lấy thông lượng trung bình (EMA)
            current_sched_list[num_active_ues].avg_throughput_kbps = mlwdf_stats[ue_id].avg_throughput_kbps;
            float R_avg = current_sched_list[num_active_ues].avg_throughput_kbps;
            if (R_avg <= 0.1) R_avg = 1.0;

            // 3. Tính trễ (HoL Delay)
            mac_rlc_status_resp_t rlc_status = mac_rlc_status_ind(
                module_idP, rnti, module_idP, frameP, subframeP,
                ENB_FLAG_YES, MBMS_FLAG_NO, 3, 0, 0
            );

            int32_t delay_ms = 0;
            if (rlc_status.bytes_in_buffer > 0) {
                uint32_t creation_time = rlc_status.head_sdu_creation_time;
                if (creation_time > 0) {
                    if (current_time_ms >= creation_time) {
                        delay_ms = current_time_ms - creation_time;
                    } else {
                        delay_ms = (current_time_ms + 10240) - creation_time;
                    }
                } else {
                    delay_ms = (rlc_status.bytes_in_buffer * 8) / R_avg;
                }
            }
            if (delay_ms <= 0) delay_ms = 1;
            current_sched_list[num_active_ues].hol_delay_ms = delay_ms;

            // 4. Tính điểm M-LWDF
            current_sched_list[num_active_ues].qos_weight_alpha = 1.5;
            current_sched_list[num_active_ues].mlwdf_metric =
                current_sched_list[num_active_ues].qos_weight_alpha * ((float)current_sched_list[num_active_ues].hol_delay_ms / R_avg) * current_sched_list[num_active_ues].inst_rate_kbps;

            // [THÊM MỚI] Đẩy giá trị vào biến toàn cục để dlsch.c lấy ra ghi file
            g_mlwdf_delay[ue_id] = current_sched_list[num_active_ues].hol_delay_ms;
            g_mlwdf_thr[ue_id]  = current_sched_list[num_active_ues].avg_throughput_kbps;
            g_mlwdf_score[ue_id] = current_sched_list[num_active_ues].mlwdf_metric;

            current_sched_list[num_active_ues].rnti = rnti;
            current_sched_list[num_active_ues].ue_id = ue_id;
            num_active_ues++;
        }
    }

    // ========================================================================
    // GIAI ĐOẠN 2: SẮP XẾP UEs THEO ĐỘ ƯU TIÊN M-LWDF (Giảm dần)
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
    // GIAI ĐOẠN 3: CẬP NHẬT DANH SÁCH & GỌI TIỀN XỬ LÝ GỐC
    // ========================================================================
    if (num_active_ues > 0) {
        // Log hiển thị trên màn hình Terminal
        printf("[MAC-MLWDF] Lập lịch %d UEs | Top 1 UE: RNTI %x, Điểm: %.2f, Trễ: %d ms, CQI: %d\n",
            num_active_ues, current_sched_list[0].rnti, current_sched_list[0].mlwdf_metric,
            current_sched_list[0].hol_delay_ms, UE_info->UE_sched_ctrl[current_sched_list[0].ue_id].dl_cqi[CC_id]);

        // 1. Tráo đổi con trỏ danh sách liên kết của OAI
        UE_info->list.head = current_sched_list[0].ue_id;
        for (int i = 0; i < num_active_ues - 1; i++) {
            UE_info->list.next[current_sched_list[i].ue_id] = current_sched_list[i+1].ue_id;
        }
        UE_info->list.next[current_sched_list[num_active_ues - 1].ue_id] = -1;

        // 2. Cập nhật EMA Throughput: R_avg(t) = (1-beta)*R_avg(t-1) + beta*r_i(t)
        for (int i = 0; i < num_active_ues; i++) {
            int id = current_sched_list[i].ue_id;
            mlwdf_stats[id].avg_throughput_kbps =
                (0.99 * mlwdf_stats[id].avg_throughput_kbps) + (0.01 * current_sched_list[i].inst_rate_kbps);
        }
    }

    // 3. Gọi hàm lập lịch gốc của OAI (Sẽ cấp phát PRB dựa trên thứ tự danh sách mới)
    schedule_ue_spec(module_idP, CC_id, frameP, subframeP);
}
