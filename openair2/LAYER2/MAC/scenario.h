#ifndef SCENARIO_H
#define SCENARIO_H

// Định nghĩa các hằng số cho thuật toán
#define SCHEDULER_ROUND_ROBIN 1
#define SCHEDULER_MAX_CI      2
#define SCHEDULER_PF          3
#define SCHEDULER_QOS_AWARE   4
#define SCHEDULER_AI_AGENT    5

// Cấu trúc cấu hình chạy mô phỏng
typedef struct {
    int active_scheduler;
    int num_ues;
    int bandwidth_mhz;
    // ... các config khác
} SimulationConfig;

extern SimulationConfig sim_config;

// Hàm lấy thuật toán đang được cấu hình
static inline int Get_Simulation_Config_Scheduler() {
    return sim_config.active_scheduler; // Đọc từ biến toàn cục
}

#endif