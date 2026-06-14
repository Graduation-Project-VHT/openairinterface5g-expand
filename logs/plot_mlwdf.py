import os

import matplotlib
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

matplotlib.use("Agg")

# ==========================================
# 1. CONFIGURATION
# ==========================================
CSV_FILE = "kiet_RR-10UE-test.csv"  # Đã cập nhật file của 10 UEs
EXPERIMENT_NAME = "RR Scheduler"

OUTPUT_DIR = f"figures_{EXPERIMENT_NAME.replace(' ', '_')}"
os.makedirs(OUTPUT_DIR, exist_ok=True)

plt.rcParams.update(
    {
        "font.family": "monospace",
        "font.size": 10,
        "axes.labelsize": 10,
        "axes.titlesize": 11,
        "axes.titleweight": "bold",
        "legend.fontsize": 9,
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "figure.autolayout": True,
    }
)


def calculate_jains_fairness(throughput_array):
    sum_tput = np.sum(throughput_array)
    sum_tput_sq = np.sum(throughput_array**2)
    if sum_tput_sq == 0:
        return 1.0
    return (sum_tput**2) / (len(throughput_array) * sum_tput_sq)


def extract_dynamic_label(ue_data, rnti):
    qos_str = "Unknown QoS"
    cqi_str = "Unknown CQI"
    bytes_per_prb = 300

    # Phân loại QoS tự động (Chuẩn với dữ liệu 10 UEs)
    if "qos_alpha" in ue_data.columns and ue_data["qos_alpha"].sum() > 0:
        qos_val = ue_data["qos_alpha"].iloc[0]
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
    elif "qci" in ue_data.columns:
        qci_val = ue_data["qci"].iloc[0]
        qos_str = f"QCI {int(qci_val)}"

    # Phân loại Vùng sóng CQI
    if "cqi_profile" in ue_data.columns:
        cqi_prof = ue_data["cqi_profile"].iloc[0]
        if cqi_prof == 0:
            cqi_str = "Center(CQI=14)"
            bytes_per_prb = 700
        elif cqi_prof == 1:
            cqi_str = "Mid(CQI=9)"
            bytes_per_prb = 300
        else:
            cqi_str = "Edge(CQI=4)"
            bytes_per_prb = 30

    label = f"UE {rnti} | {qos_str} | {cqi_str}"
    return label, bytes_per_prb


def main():
    print(f"[{EXPERIMENT_NAME}] Đang xử lý dữ liệu từ {CSV_FILE}...")
    if not os.path.exists(CSV_FILE):
        print(f"[ERROR] Cannot find {CSV_FILE}!")
        return

    df = pd.read_csv(CSV_FILE)
    df = df[df["timestamp_ms"] > 0].copy()
    if df.empty:
        return

    if "tbs_bytes" not in df.columns:
        df["tbs_bytes"] = 0

    # Trục thời gian chuẩn liên tục
    min_time = int(df["timestamp_ms"].min())
    max_time = int(df["timestamp_ms"].max())
    full_time_index = np.arange(min_time, max_time + 1)

    WINDOW = 100
    unique_rntis = sorted(df["rnti"].unique())

    # Khởi tạo Palette màu cho 10-20 UEs không bị trùng
    cmap = plt.colormaps.get_cmap("tab20")
    line_styles = ["-", "--", "-.", ":"]
    ue_styles = {}
    for i, rnti in enumerate(unique_rntis):
        ue_styles[rnti] = {
            "color": cmap(i % 20),
            "style": line_styles[i % len(line_styles)],
        }

    sns.set_theme(style="whitegrid", rc=plt.rcParams)
    legend_kwargs = {
        "bbox_to_anchor": (1.02, 1),
        "loc": "upper left",
        "borderaxespad": 0.0,
    }

    processed_data = {}

    for rnti in unique_rntis:
        ue_df = df[df["rnti"] == rnti].copy()
        ue_df = ue_df.sort_values(by="timestamp_ms")

        label_name, bytes_per_prb = extract_dynamic_label(ue_df, rnti)

        # ----------------------------------------------------
        # LUỒNG A: XỬ LÝ DELAY (KHÔNG REINDEX)
        # Nối trực tiếp các điểm dữ liệu thực để biểu đồ mượt, dốc chuẩn
        # ----------------------------------------------------
        delay_df = ue_df[["timestamp_ms", "hol_delay_ms"]].copy()
        delay_df["hol_delay_ms"] = delay_df["hol_delay_ms"].replace(0, np.nan)
        delay_df = delay_df.dropna(subset=["hol_delay_ms"])

        # ----------------------------------------------------
        # LUỒNG B: XỬ LÝ THROUGHPUT & PRB (CÓ REINDEX)
        # Ép các TTI trống về 0 để vẽ đồ thị mật độ chính xác
        # ----------------------------------------------------
        thr_df = ue_df.drop_duplicates(subset=["timestamp_ms"]).copy()
        thr_df.set_index("timestamp_ms", inplace=True)
        thr_df = thr_df.reindex(full_time_index)

        thr_df["nb_rb"] = thr_df["nb_rb"].fillna(0)
        thr_df["fake_thr_kbps"] = (thr_df["nb_rb"] * bytes_per_prb * 8) / 1.0

        processed_data[rnti] = {
            "label": label_name,
            "color": ue_styles[rnti]["color"],
            "style": ue_styles[rnti]["style"],
            # Data cho Hình 1 (Delay)
            "delay_time": delay_df["timestamp_ms"],
            "raw_delay": delay_df["hol_delay_ms"],
            # Data cho Hình 2 & 4 (Throughput, PRB)
            "full_time": full_time_index,
            "smooth_thr": thr_df["fake_thr_kbps"]
            .rolling(window=WINDOW, min_periods=1)
            .mean(),
            "smooth_rb": thr_df["nb_rb"].rolling(window=WINDOW, min_periods=1).mean(),
        }

    # ==========================================
    # FIGURE 1: AVERAGE HoL DELAY
    # ==========================================
    fig1, ax1 = plt.subplots(figsize=(10, 5))
    if df["hol_delay_ms"].max() == 0:
        ax1.text(0.5, 0.5, "HoL Delay Not Logged", transform=ax1.transAxes, color="red")

    for rnti, data in processed_data.items():
        ax1.plot(
            data["delay_time"],
            data["raw_delay"],
            label=data["label"],
            color=data["color"],
            linestyle=data["style"],
            linewidth=2.0,
            alpha=0.9,
        )

    ax1.set_title(f"[{EXPERIMENT_NAME}] Head-of-Line Delay Stratification", loc="left")
    ax1.set_xlabel("Simulation Time (ms)")
    ax1.set_ylabel("Average HoL Delay (ms)")
    ax1.legend(**legend_kwargs)
    fig1.savefig(f"{OUTPUT_DIR}/1_Delay_Time_Series.png", dpi=300, bbox_inches="tight")
    plt.close(fig1)

    # ==========================================
    # FIGURE 2: INSTANTANEOUS THROUGHPUT
    # ==========================================
    fig2, ax2 = plt.subplots(figsize=(10, 5))
    for rnti, data in processed_data.items():
        ax2.plot(
            data["full_time"],
            data["smooth_thr"],
            label=data["label"],
            color=data["color"],
            linestyle=data["style"],
            linewidth=2.0,
            alpha=0.9,
        )

    ax2.set_title(
        f"[{EXPERIMENT_NAME}] Actual Physical Throughput (Dynamic CQI Mapping)",
        loc="left",
    )
    ax2.set_xlabel("Simulation Time (ms)")
    ax2.set_ylabel("Throughput (kbps)")
    ax2.legend(**legend_kwargs)
    fig2.savefig(
        f"{OUTPUT_DIR}/2_Throughput_Time_Series.png", dpi=300, bbox_inches="tight"
    )
    plt.close(fig2)

    # ==========================================
    # FIGURE 3: USER PERCEIVED THROUGHPUT & FAIRNESS
    # ==========================================
    fig3, ax3 = plt.subplots(figsize=(10, 5))

    grouped = df.groupby("rnti")
    tbs_sum = grouped["tbs_bytes"].sum()
    time_min = grouped["timestamp_ms"].min()
    time_max = grouped["timestamp_ms"].max()

    active_time_s = (time_max - time_min + 1) / 1000.0
    perceived_mbps = ((tbs_sum * 8) / active_time_s) / 1_000_000

    ue_stats = pd.DataFrame(
        {"rnti": tbs_sum.index, "perceived_mbps": perceived_mbps.values}
    )

    ue_stats["label"] = ue_stats["rnti"].apply(lambda r: processed_data[r]["label"])
    jains_idx = calculate_jains_fairness(ue_stats["perceived_mbps"].values)

    bar_palette = {rnti: ue_styles[rnti]["color"] for rnti in unique_rntis}
    sns.barplot(
        data=ue_stats,
        x="rnti",
        y="perceived_mbps",
        hue="rnti",
        palette=bar_palette,
        dodge=False,
        ax=ax3,
        legend=False,
    )

    ax3.set_title(
        f"[{EXPERIMENT_NAME}] User Perceived Throughput (Jain's Index: {jains_idx:.3f})"
    )
    ax3.set_ylabel("Perceived Throughput (Mbps)")
    ax3.set_xlabel("User Equipment (RNTI)")
    ax3.set_xticklabels(ue_stats["label"], rotation=45, ha="right", fontsize=8)

    for container in ax3.containers:
        ax3.bar_label(container, fmt="%.2f", padding=2, fontsize=8)

    fig3.savefig(
        f"{OUTPUT_DIR}/3_Perceived_Throughput_Fairness.png",
        dpi=300,
        bbox_inches="tight",
    )
    plt.close(fig3)

    # ==========================================
    # FIGURE 4: PRB ALLOCATION
    # ==========================================
    if "nb_rb" in df.columns:
        fig4, ax4 = plt.subplots(figsize=(10, 5))
        for rnti, data in processed_data.items():
            ax4.plot(
                data["full_time"],
                data["smooth_rb"],
                label=data["label"],
                color=data["color"],
                linestyle=data["style"],
                linewidth=2.0,
                alpha=0.9,
            )

        ax4.set_title(
            f"[{EXPERIMENT_NAME}] Physical Resource Block (PRB) Allocation", loc="left"
        )
        ax4.set_xlabel("Simulation Time (ms)")
        ax4.set_ylabel("Assigned PRBs per TTI")
        ax4.legend(**legend_kwargs)
        fig4.savefig(f"{OUTPUT_DIR}/4_PRB_Allocation.png", dpi=300, bbox_inches="tight")
        plt.close(fig4)

    print(f"[SUCCESS] All figures exported to: {OUTPUT_DIR}/")


if __name__ == "__main__":
    main()
