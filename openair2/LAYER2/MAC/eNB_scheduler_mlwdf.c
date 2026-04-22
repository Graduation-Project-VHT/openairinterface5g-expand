#include <stdio.h>  // Thêm thư viện này để hỗ trợ fopen, fprintf, fclose
#include <stdlib.h>
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

// ========================================================================
// BIẾN TOÀN CỤC (Cầu nối ghi log & Lưu trữ Profile cố định cho UE)
// ========================================================================
int g_mlwdf_delay[MAX_MOBILES_PER_ENB] = {0};
float g_mlwdf_thr[MAX_MOBILES_PER_ENB] = {0.0};
float g_mlwdf_score[MAX_MOBILES_PER_ENB] = {0.0};

// Mảng lưu "Định mệnh" của từng UE khi vừa bật trạm eNB
float g_ue_qos_alpha[MAX_MOBILES_PER_ENB] = {0.0};
int   g_ue_cqi_profile[MAX_MOBILES_PER_ENB] = {0};

mlwdf_ue_stats_t mlwdf_stats[MAX_MOBILES_PER_ENB];

// ========================================================================
// HÀM KHỞI TẠO: Gán ngẫu nhiên Profile Dịch vụ & Sóng cho quy mô N UEs
// ========================================================================
void init_mlwdf_scheduler(void) {
    // ---------------------------------------------------------
    // [TỰ ĐỘNG DỌN DẸP FILE LOG]
    // Mở file ở chế độ "w" (write) để xóa sạch dữ liệu cũ
    // và ghi dòng tiêu đề mới cho lần chạy mô phỏng này.
    // ---------------------------------------------------------
    FILE *f = fopen("scheduler_log.csv", "w");
    if (f != NULL) {
        fprintf(f, "timestamp_ms,frame,subframe,rnti,direction,nb_rb,mcs,tbs_bytes,cqi,retx,hol_delay_ms,avg_thr_kbps,mlwdf_score,qos_alpha,cqi_profile\n");
        fclose(f);
    } else {
        printf("[MAC-MLWDF] LỖI: Không thể tạo hoặc ghi đè file scheduler_log.csv!\n");
    }

    // 5 Mức QoS Tiêu biểu trong mạng 4G/5G
    float qos_pool[5] = {
        3.0, // 1. VoIP / URLLC: Cực kỳ nhạy cảm với trễ (Ưu tiên tối thượng)
        2.0, // 2. Video Call / Cloud Gaming: Rất nhạy cảm với trễ
        1.2, // 3. Web Browsing / Social Media: Nhạy cảm trễ trung bình
        0.8, // 4. File Transfer (FTP/HTTP): Chịu trễ tốt, cần thông lượng
        0.4  // 5. Background / IoT Sensor: Không quan tâm trễ (Ưu tiên thấp nhất)
    };

    printf("\n======================================================\n");
    printf("[MAC-MLWDF] KHOI TAO BANG PROFILE QoS VA SONG CHO UE\n");
    printf("======================================================\n");

    for (int i = 0; i < MAX_MOBILES_PER_ENB; i++) {
        mlwdf_stats[i].rnti = 0;
        mlwdf_stats[i].avg_throughput_kbps = 1.0;
        mlwdf_stats[i].hol_delay_ms = 0;

        g_mlwdf_delay[i] = 0;
        g_mlwdf_thr[i] = 0.0;
        g_mlwdf_score[i] = 0.0;

        // "Rút thăm" ngẫu nhiên 1 trong 5 dịch vụ cho Slot UE này
        g_ue_qos_alpha[i] = qos_pool[rand() % 5];

        // "Rút thăm" ngẫu nhiên vị trí đứng của UE (Môi trường vô tuyến)
        // 0: Đứng gần trạm (Sóng tốt)
        // 1: Đứng giữa Cell (Sóng trung bình)
        // 2: Đứng rìa Cell hoặc di chuyển nhanh (Sóng yếu)
        g_ue_cqi_profile[i] = rand() % 3;

        // In log để khi chạy eNB, dễ dàng theo dõi
        printf("[MLWDF-INIT] Slot UE_ID %d | QoS Alpha: %.1f | Vung Song: %d\n", i, g_ue_qos_alpha[i], g_ue_cqi_profile[i]);
    }
    printf("======================================================\n\n");
}

// ========================================================================
// [HÀM MỚI] Giả lập Hiệu ứng cái bóng (Shadowing/Fading)
// ========================================================================
void generate_dynamic_cqi(module_id_t module_idP) {
    eNB_MAC_INST *eNB = RC.mac[module_idP];
    UE_info_t *UE_info = &eNB->UE_info;
    int CC_id = 0;

    for (int ue_id = 0; ue_id < MAX_MOBILES_PER_ENB; ue_id++) {
        if (UE_info->active[ue_id] == 1) {

            uint8_t current_cqi = UE_info->UE_sched_ctrl[ue_id].dl_cqi[CC_id];
            int profile = g_ue_cqi_profile[ue_id];

            // Nếu CQI đang là lý tưởng (chưa bị nhiễu), thiết lập base theo Vùng sóng
            if (current_cqi == 0 || current_cqi >= 15) {
                if (profile == 0) current_cqi = 14;      // Vùng tốt
                else if (profile == 1) current_cqi = 9;  // Vùng khá
                else current_cqi = 5;                    // Vùng kém
            }

            // TẠO BƯỚC NHẢY FADING ngẫu nhiên (Delta: -1, 0, +1)
            int delta = (rand() % 3) - 1;
            int new_cqi = current_cqi + delta;

            // Ép biên độ dao động để UE không thoát khỏi "Vùng sóng" của nó
            if (profile == 0) {
                if (new_cqi > 15) new_cqi = 15;
                if (new_cqi < 12) new_cqi = 12;
            } else if (profile == 1) {
                if (new_cqi > 11) new_cqi = 11;
                if (new_cqi < 7)  new_cqi = 7;
            } else {
                if (new_cqi > 6) new_cqi = 6;
                if (new_cqi < 3) new_cqi = 3;
            }

            // Ghi đè CQI nhiễu vào Tầng MAC
            UE_info->UE_sched_ctrl[ue_id].dl_cqi[CC_id] = new_cqi;
        }
    }
}

// Hàm Lập lịch QoS Aware M-LWDF
void schedule_ue_spec_mlwdf(module_id_t module_idP, int CC_id, frame_t frameP, sub_frame_t subframeP) {

    // ========================================================================
    // [THỦ THUẬT MỚI] LAZY INITIALIZATION
    // ========================================================================
    static int is_mlwdf_initialized = 0;
    if (is_mlwdf_initialized == 0) {
        init_mlwdf_scheduler();
        is_mlwdf_initialized = 1;
    }

    eNB_MAC_INST *eNB = RC.mac[module_idP];
    UE_info_t *UE_info = &eNB->UE_info;

    int num_active_ues = 0;
    int ue_id;
    mlwdf_ue_stats_t current_sched_list[MAX_MOBILES_PER_ENB];
    uint32_t current_time_ms = (frameP * 10) + subframeP;

    if (frameP > 1000) {
        generate_dynamic_cqi(module_idP);
    }

    // ========================================================================
    // GIAI ĐOẠN 1: TÍNH TOÁN METRIC CHO TỪNG UE ĐANG HOẠT ĐỘNG
    // ========================================================================
    for (ue_id = 0; ue_id < MAX_MOBILES_PER_ENB; ue_id++) {

        if (UE_info->active[ue_id] == 1) {
            rnti_t rnti = UE_RNTI(module_idP, ue_id);
            if (rnti == 0 || rnti == NOT_A_RNTI) continue;

            UE_sched_ctrl_t *ue_sched_ctrl = &UE_info->UE_sched_ctrl[ue_id];

            // 1. Tính tốc độ tức thời r_i(t)
            uint8_t cqi = ue_sched_ctrl->dl_cqi[CC_id];
            if (cqi == 0) cqi = 5;
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
                    if (current_time_ms >= creation_time) delay_ms = current_time_ms - creation_time;
                    else delay_ms = (current_time_ms + 10240) - creation_time;
                } else {
                    delay_ms = (rlc_status.bytes_in_buffer * 8) / R_avg;
                }
            }
            if (delay_ms <= 0) delay_ms = 1;
            current_sched_list[num_active_ues].hol_delay_ms = delay_ms;

            // 4. Tính điểm M-LWDF bằng Profile cố định đã cấp lúc khởi tạo trạm
            current_sched_list[num_active_ues].qos_weight_alpha = g_ue_qos_alpha[ue_id];

            current_sched_list[num_active_ues].mlwdf_metric =
                current_sched_list[num_active_ues].qos_weight_alpha * ((float)current_sched_list[num_active_ues].hol_delay_ms / R_avg) * current_sched_list[num_active_ues].inst_rate_kbps;

            // Lưu log CSV (Truyền giá trị sang mảng toàn cục)
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
        // 1. Tráo đổi con trỏ danh sách liên kết của OAI
        UE_info->list.head = current_sched_list[0].ue_id;
        for (int i = 0; i < num_active_ues - 1; i++) {
            UE_info->list.next[current_sched_list[i].ue_id] = current_sched_list[i+1].ue_id;
        }
        UE_info->list.next[current_sched_list[num_active_ues - 1].ue_id] = -1;

        // 2. Cập nhật EMA Throughput
        for (int i = 0; i < num_active_ues; i++) {
            int id = current_sched_list[i].ue_id;
            mlwdf_stats[id].avg_throughput_kbps =
                (0.99 * mlwdf_stats[id].avg_throughput_kbps) + (0.01 * current_sched_list[i].inst_rate_kbps);
        }
    }

    // 3. Gọi hàm lập lịch gốc của OAI
    schedule_ue_spec(module_idP, CC_id, frameP, subframeP);
}
