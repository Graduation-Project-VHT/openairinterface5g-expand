#ifndef ENB_SCHEDULER_MLWDF_H
#define ENB_SCHEDULER_MLWDF_H

#include "mac.h"
#include "mac_extern.h"
#include "PHY/defs_eNB.h"

// Cấu trúc lưu trữ các thông số tính toán M-LWDF cho từng UE
typedef struct {
    rnti_t rnti;                // Định danh của UE trong cell
    int ue_id;                  // Index của UE trong mảng MAC của eNB
    uint32_t hol_delay_ms;      // Trễ Head-of-Line (W_i(t))
    float avg_throughput_kbps;  // Thông lượng trung bình (\overline{R}_i(t))
    float inst_rate_kbps;       // Tốc độ truyền tải tức thời (r_i(t))
    float qos_weight_alpha;     // Trọng số QoS (\alpha_i)
    float mlwdf_metric;         // Điểm số ưu tiên M-LWDF
} mlwdf_ue_stats_t;

// Hàm khởi tạo bộ lập lịch (dùng để reset thông lượng khi mới bật trạm)
void init_mlwdf_scheduler(void);

// Hàm lập lịch chính của thuật toán M-LWDF (Đã chuẩn hóa tham số)
void schedule_ue_spec_mlwdf(module_id_t module_idP, frame_t frameP, sub_frame_t subframeP, int *mbsfn_flag);

#endif // ENB_SCHEDULER_MLWDF_H
