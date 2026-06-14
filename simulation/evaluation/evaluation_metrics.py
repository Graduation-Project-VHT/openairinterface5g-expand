import pandas as pd
import numpy as np
from pathlib import Path

def compute_metrics(csv_path: Path) -> dict:
    df = pd.read_csv(csv_path)
    # Deduplicate
    df = (df.sort_values('nb_rb', ascending=False)
            .drop_duplicates(subset=['timestamp_ms', 'rnti'])
            .sort_values('timestamp_ms'))

    duration_s = (df['timestamp_ms'].max() - df['timestamp_ms'].min()) / 1000

    per_ue_tbs = (df.groupby('rnti')['tbs_bytes']
                    .sum() * 8 / duration_s / 1000)  # kbps

    n = len(per_ue_tbs)
    x = per_ue_tbs.values
    jfi = x.sum()**2 / (n * (x**2).sum())

    return {
        "scheduler":         "RR",   # change per run
        "cell_tput_kbps":    round(x.sum(), 2),
        "per_ue_tput_kbps":  per_ue_tbs.to_dict(),
        "tput_std_kbps":     round(x.std(), 2),
        "jains_fairness":    round(jfi, 6),
        "spectral_eff_bpHz": round(x.sum() / 1000 / 5.0, 4),  # 5MHz
        "duration_s":        round(duration_s, 2),
    }

if __name__ == "__main__":
    m = compute_metrics("../logs/DL_scheduler_log.csv")
    for k, v in m.items():
        print(f"{k:25s}: {v}")
