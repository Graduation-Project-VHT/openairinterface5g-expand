import os

import matplotlib
import numpy as np  # Thêm thư viện Toán học để xử lý trục thời gian tuyến tính
import pandas as pd

matplotlib.use("Agg")  # Ép hệ thống chạy ngầm, không mở cửa sổ UI để tránh lỗi Segfault
import matplotlib.pyplot as plt

csv_file = "scheduler_log.csv"

# ==========================================
# 1. KIỂM TRA VÀ ĐỌC DỮ LIỆU
# ==========================================
if not os.path.exists(csv_file):
    print(f"[LỖI] Không tìm thấy file '{csv_file}' trong thư mục này.")
    print("Vui lòng đảm bảo bạn đang đứng đúng thư mục chứa file log!")
    exit()

print("Đang đọc và xử lý dữ liệu từ scheduler_log.csv...")
df = pd.read_csv(csv_file)

if df.empty:
    print(
        "[LỖI] File CSV tồn tại nhưng trống không (Không có dữ liệu). Hãy chạy lại mô phỏng!"
    )
    exit()

# Lấy danh sách ID của các thiết bị
rntis = df["rnti"].unique()

# ==========================================
# 2. CẤU HÌNH GIAO DIỆN BIỂU ĐỒ (THESIS STYLE)
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

fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 11), sharex=True)
fig.suptitle(
    "ĐÁNH GIÁ HIỆU NĂNG THUẬT TOÁN LẬP LỊCH M-LWDF TẠI TẦNG MAC",
    fontsize=18,
    fontweight="bold",
    y=0.97,
)

# Bảng màu chuẩn khoa học (Tối đa 7 UEs không bị trùng màu)
colors = ["#e63946", "#1d3557", "#2a9d8f", "#f4a261", "#9c6644", "#8338ec", "#ff006e"]
line_styles = ["-", "--", "-.", ":", "-", "--", "-."]

# ==========================================
# 3. VẼ ĐỒ THỊ CHO TỪNG UE (TRỤC THỜI GIAN TUYẾN TÍNH)
# ==========================================
for i, rnti in enumerate(rntis):
    ue_data = df[df["rnti"] == rnti]

    # KỸ THUẬT DUỖI THẲNG TRỤC X:
    # Đếm số lượng log của UE này, mỗi dòng là 1ms. Đem chia 1000 ra Giây tuyệt đối.
    monotonic_time_sec = np.arange(len(ue_data)) / 1000.0

    # Lấy thông số để làm Nhãn (Label)
    alpha = ue_data["qos_alpha"].iloc[0]
    profile = ue_data["cqi_profile"].iloc[0]
    label_name = f"UE {rnti} (QoS \u03b1={alpha:.1f}, Vùng sóng={profile})"

    color = colors[i % len(colors)]
    style = line_styles[i % len(line_styles)]

    # Biểu đồ 1: HoL Delay
    ax1.plot(
        monotonic_time_sec,
        ue_data["hol_delay_ms"],
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=1.8,
        alpha=0.9,
    )

    # Biểu đồ 2: M-LWDF Score
    ax2.plot(
        monotonic_time_sec,
        ue_data["mlwdf_score"],
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=1.8,
        alpha=0.9,
    )

    # Biểu đồ 3: PRB Allocation (Tài nguyên cấp phát)
    ax3.plot(
        monotonic_time_sec,
        ue_data["nb_rb"],
        label=label_name,
        color=color,
        linestyle=style,
        linewidth=1.8,
        alpha=0.8,
    )

# ==========================================
# 4. TRANG TRÍ VÀ CĂN CHỈNH CHI TIẾT
# ==========================================
# Giao diện trục 1
ax1.set_ylabel("HoL Delay (ms)", fontweight="bold")
ax1.set_title(
    "1. Tích tụ Độ trễ của Gói tin đầu hàng đợi (Head-of-Line Delay)",
    loc="left",
    color="#333333",
)
ax1.grid(True, linestyle="--", alpha=0.5)
ax1.legend(
    loc="upper left", bbox_to_anchor=(1.01, 1), borderaxespad=0.0
)  # Đẩy legend ra ngoài viền phải

# Giao diện trục 2
ax2.set_ylabel("M-LWDF Metric Score", fontweight="bold")
ax2.set_title(
    "2. Điểm số Cạnh tranh (Bùng nổ khi Delay vượt giới hạn)",
    loc="left",
    color="#333333",
)
ax2.grid(True, linestyle="--", alpha=0.5)

# Giao diện trục 3
ax3.set_ylabel("Số PRB Cấp phát", fontweight="bold")
ax3.set_xlabel("Thời gian mô phỏng tuyến tính (Giây)", fontweight="bold")
ax3.set_title(
    "3. Băng thông Vật lý (Khối tài nguyên PRB) được chia", loc="left", color="#333333"
)
ax3.grid(True, linestyle="--", alpha=0.5)

# Tự động căn lề để không bị cắt xén chữ
plt.tight_layout(
    rect=[0, 0.02, 0.85, 0.95]
)  # Dành khoảng trống bên phải (0.85) cho bảng Chú thích

# ==========================================
# 5. XUẤT FILE ẢNH
# ==========================================
output_filename = "mlwdf_results_linear_pro.png"
plt.savefig(output_filename, dpi=300, bbox_inches="tight")
print(f"[THÀNH CÔNG] Đã render xong biểu đồ tuyến tính chất lượng cao!")
print(f"--> Hãy mở file '{output_filename}' để kiểm tra kết quả.")
