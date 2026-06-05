import os

import matplotlib
import numpy as np
import pandas as pd
import matplotlib.cm as cm

matplotlib.use("Agg")
import matplotlib.pyplot as plt

csv_file = "DL_scheduler_log.csv"

# ==========================================
# 1. READ AND RESTORE DATA
# ==========================================
if not os.path.exists(csv_file):
    print(f"[ERROR] Cannot find {csv_file}!")
    exit()

df = pd.read_csv(csv_file)
if df.empty:
    print(f"[ERROR] The file {csv_file} is empty!")
    exit()

rntis = df["rnti"].unique()

# ==========================================
# 2. THESIS-STYLE PLOT CONFIGURATION (4 GRAPHS)
# ==========================================
plt.rcParams.update(
    {
        "font.size": 11,
        "font.family": "sans-serif",
        "axes.labelsize": 12,
        "axes.titlesize": 13,
        "legend.fontsize": 10,
    }
)

fig, (ax1, ax2, ax3, ax4) = plt.subplots(4, 1, figsize=(14, 14), sharex=True)
fig.suptitle(
    "PERFORMANCE EVALUATION OF M-LWDF SCHEDULER (RANDOMIZED SCENARIO)",
    fontsize=18,
    fontweight="bold",
    y=0.97,
)

_cmap = cm.get_cmap("tab20", 20)
LINE_STYLES = ["-", "--", "-.", ":"]

# ==========================================
# 3. DATA PROCESSING & PLOTTING
# ==========================================
for i, rnti in enumerate(rntis):
    ue_data = df[df["rnti"] == rnti].copy()
    if ue_data.empty:
        continue

    time_sec = ue_data["timestamp_ms"].values / 1000.0

    # DYNAMIC LABEL EXTRACTION (Reading from CSV directly)
    # Lấy giá trị QoS và CQI Profile thực tế của UE này từ dòng đầu tiên
    qos_val = ue_data["qos_alpha"].iloc[0]
    cqi_prof = int(ue_data["cqi_profile"].mode()[0])

    # Xác định danh tính QoS (Đã cập nhật 5 mức)
    if abs(qos_val - 5.0) < 0.1:
        qos_str = "URLLC (\u03b1=5.0)"
    elif abs(qos_val - 3.0) < 0.1:
        qos_str = "VIP (\u03b1=3.0)"
    elif abs(qos_val - 1.2) < 0.1:
        qos_str = "Norm (\u03b1=1.2)"
    elif abs(qos_val - 0.5) < 0.1:
        qos_str = "Sync (\u03b1=0.5)"
    else:
        qos_str = "BE (\u03b1=0.1)"

    # Xác định danh tính Vùng sóng & Sức chứa PRB
    if cqi_prof == 0:
        cqi_str = "Center(CQI≥12)"
    elif cqi_prof == 1:
        cqi_str = "Mid(CQI 7-11)"
    else:
        cqi_str = "Edge(CQI≤6)"

    label_name = f"UE {rnti} | {qos_str} | {cqi_str}"

    color = _cmap(i % 20)
    style = LINE_STYLES[(i // 20) % len(LINE_STYLES)]

    # Lọc các giá trị 0 do không có gói tin
    ue_data["hol_delay_ms"] = ue_data["hol_delay_ms"].replace(0, np.nan)
    ue_data["mlwdf_score"] = ue_data["mlwdf_score"].replace(0, np.nan)

    # Tính Throughput Vật lý thực tế (Mbps)
    ue_data["real_thr_mbps"] = ue_data["tbs_bytes"] * 8.0 / 1000.0

    # Làm mượt (Moving Average)
    window_size = 30
    smooth_delay = (
        ue_data["hol_delay_ms"].rolling(window=window_size, min_periods=1).mean()
    )
    smooth_score = (
        ue_data["mlwdf_score"].rolling(window=window_size, min_periods=1).mean()
    )
    smooth_rb = ue_data["nb_rb"].rolling(window=window_size, min_periods=1).mean()
    smooth_thr_mbps = (
        ue_data["real_thr_mbps"].rolling(window=window_size, min_periods=1).mean()
    )

    # Vẽ 4 đồ thị
    ax1.plot(
        time_sec,
        smooth_delay,
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=2.2,
        alpha=0.95,
    )
    ax2.plot(
        time_sec,
        smooth_score,
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=2.2,
        alpha=0.95,
    )
    ax3.plot(
        time_sec,
        smooth_rb,
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=2.0,
        alpha=0.85,
    )
    ax4.plot(
        time_sec,
        smooth_thr_mbps,
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=2.2,
        alpha=0.95,
    )

# ==========================================
# 4. DECORATION & EXPORT
# ==========================================
ax1.set_ylabel("Average HoL Delay (ms)", fontweight="bold")
ax1.set_title("1. Head-of-Line Delay Stratification", loc="left", color="#333333")
ax1.grid(True, linestyle="--", alpha=0.5)
ax1.legend(loc="upper left", bbox_to_anchor=(1.01, 1), borderaxespad=0.0)

ax2.set_ylabel("M-LWDF Score", fontweight="bold")
ax2.set_title("2. M-LWDF Metric Score", loc="left", color="#333333")
ax2.grid(True, linestyle="--", alpha=0.5)

ax3.set_ylabel("Avg PRB Allocation", fontweight="bold")
ax3.set_title(
    "3. Physical Resource Block (PRB) Allocation Frequency", loc="left", color="#333333"
)
ax3.grid(True, linestyle="--", alpha=0.5)

ax4.set_ylabel("Actual Throughput (Mbps)", fontweight="bold")
ax4.set_xlabel("Simulation Time (Seconds)", fontweight="bold")
ax4.set_title(
    "4. Actual Physical Throughput (Dynamic CQI Mapping)", loc="left", color="#333333"
)
ax4.grid(True, linestyle="--", alpha=0.5)

plt.tight_layout(
    rect=[0, 0.02, 0.82, 0.95]
)  # Nới lề phải thêm một chút cho cái Legend dài
output_filename = "mlwdf_results_random.png"
plt.savefig(output_filename, dpi=300, bbox_inches="tight")
print(f"[SUCCESS] Rendered Randomized Scenario: '{output_filename}'")
