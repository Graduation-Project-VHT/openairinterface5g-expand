#!/usr/bin/env python3
"""
Tails the OAI scheduler CSV log and pushes metrics to Prometheus Pushgateway.

Usage:
    export PUSHGATEWAY_URL="https://push.lukaxzs.myaddr.io"
    export LTE_SIM_MEMBER="kiet"
    export CSV_FILE="/openairinterface5g/logs/scheduler_log.csv"
    python3 push_metrics.py
"""

import os
import sys
import time
import csv
import requests
import signal
from collections import defaultdict

# ── Configuration ──────────────────────────────────────────────────────────
PUSHGATEWAY_URL  = os.environ.get("PUSHGATEWAY_URL",  "https://push.lukaxzs.myaddr.io")
MEMBER           = os.environ.get("LTE_SIM_MEMBER",   "kiet")
CSV_FILE         = os.environ.get("CSV_FILE",         "../logs/DL_scheduler_log.csv")
PUSH_INTERVAL    = float(os.environ.get("PUSH_INTERVAL", "1.0"))  # seconds between pushes

# ── Graceful shutdown ───────────────────────────────────────────────────────
def _shutdown(sig, frame):
    global running
    print("\n[pusher] Shutting down, clearing metrics from Pushgateway...")
    running = False
    delete_old_metrics()
    sys.exit(0)
running = True

signal.signal(signal.SIGINT,  _shutdown)
signal.signal(signal.SIGTERM, _shutdown)

# ── CSV column indices (matches your header exactly) ───────────────────────
# timestamp_ms, frame, subframe, rnti, direction, nb_rb, rb_util(%), mcs,
# tbs_bytes, sdu_bytes, cqi, retx, harq_pid
COL = {
    "timestamp_ms": 0,
    "frame":        1,
    "subframe":     2,
    "rnti":         3,
    "direction":    4,
    "nb_rb":        5,
    "rb_util":      6,
    "mcs":          7,
    "tbs_bytes":    8,
    "sdu_bytes":    9,
    "cqi":          10,
    "retx":         11,
    "harq_pid":     12,
}

# ── Per-UE state ────────────────────────────────────────────────────────────
class UEStats:
    """Holds the latest gauge values and running counters for one UE."""
    def __init__(self):
        # Gauges — latest value per TTI
        self.cqi      = 0
        self.mcs      = 0
        self.nb_rb    = 0
        self.rb_util  = 0.0
        self.tbs      = 0
        self.sdu      = 0
        self.harq_pid = 0
        self.retx_round = 0
        # Counters — monotonically increasing
        self.total_tbs  = 0
        self.total_tx   = 0
        self.total_retx = 0

stats = defaultdict(UEStats)  # key = (rnti, direction)

# ── Prometheus text format builder ─────────────────────────────────────────
def build_payload():
    lines = []

    # HELP and TYPE declared once per metric at the top
    metrics = [
        ("lte_mac_cqi",        "gauge",   "Channel Quality Indicator (0=unknown, 15=best)"),
        ("lte_mac_mcs",        "gauge",   "Modulation and Coding Scheme (0-28)"),
        ("lte_mac_nb_rb",      "gauge",   "Resource Blocks allocated this TTI"),
        ("lte_mac_rb_util",    "gauge",   "RB utilization percent (0-100)"),
        ("lte_mac_tbs_bytes",  "gauge",   "Transport Block Size bytes this TTI"),
        ("lte_mac_sdu_bytes",  "gauge",   "SDU payload bytes this TTI"),
        ("lte_mac_harq_pid",   "gauge",   "HARQ process ID (0-7)"),
        ("lte_mac_retx_round", "gauge",   "Retransmission round (0=new TX)"),
        ("lte_mac_se",         "gauge",   "Spectral efficiency bps/Hz"),
        ("lte_mac_overhead",   "gauge",   "Header overhead ratio"),
        ("lte_mac_tbs_total",  "counter", "Cumulative bytes scheduled"),
        ("lte_mac_tx_total",   "counter", "Total scheduling decisions"),
        ("lte_mac_retx_total", "counter", "Total retransmissions"),
    ]

    for name, mtype, help_text in metrics:
        lines.append(f"# HELP {name} {help_text}")
        lines.append(f"# TYPE {name} {mtype}")

    lines.append("")

    # Then all sample lines for all UEs
    for (rnti, direction), s in stats.items():
        lbl = f'rnti="{rnti}",dir="{direction}",member="{MEMBER}"'

        se = (s.tbs * 8.0) / (s.nb_rb * 180000.0 * 0.001) if s.nb_rb > 0 else 0.0
        overhead = 1.0 - (s.sdu / s.tbs) if s.tbs > 0 and s.sdu <= s.tbs else 0.0

        lines.append(f'lte_mac_cqi{{{lbl}}} {s.cqi}')
        lines.append(f'lte_mac_mcs{{{lbl}}} {s.mcs}')
        lines.append(f'lte_mac_nb_rb{{{lbl}}} {s.nb_rb}')
        lines.append(f'lte_mac_rb_util{{{lbl}}} {s.rb_util:.2f}')
        lines.append(f'lte_mac_tbs_bytes{{{lbl}}} {s.tbs}')
        lines.append(f'lte_mac_sdu_bytes{{{lbl}}} {s.sdu}')
        lines.append(f'lte_mac_harq_pid{{{lbl}}} {s.harq_pid}')
        lines.append(f'lte_mac_retx_round{{{lbl}}} {s.retx_round}')
        lines.append(f'lte_mac_se{{{lbl}}} {se:.4f}')
        lines.append(f'lte_mac_overhead{{{lbl}}} {overhead:.4f}')
        lines.append(f'lte_mac_tbs_total{{{lbl}}} {s.total_tbs}')
        lines.append(f'lte_mac_tx_total{{{lbl}}} {s.total_tx}')
        lines.append(f'lte_mac_retx_total{{{lbl}}} {s.total_retx}')
        lines.append("")

    return "\n".join(lines)
# ── Push to Pushgateway ─────────────────────────────────────────────────────
def push():
    if not stats:
        return  # nothing to push yet

    payload = build_payload()
    url = f"{PUSHGATEWAY_URL}/metrics/job/lte_mac/member/{MEMBER}"

    try:
        r = requests.put(url, data=payload,
                         headers={"Content-Type": "text/plain"},
                         timeout=5)
        if r.status_code not in (200, 202):
            print(f"[pusher] WARNING: Pushgateway returned {r.status_code}")
    except requests.exceptions.RequestException as e:
        print(f"[pusher] Push failed: {e}")

# ── CSV row processor ────────────────────────────────────────────────────────
def process_row(row):
    """Parse one CSV data row and update the corresponding UE's stats."""
    if len(row) < 13:
        return  # malformed row, skip

    try:
        rnti      = row[COL["rnti"]].strip()
        direction = row[COL["direction"]].strip()
        key       = (rnti, direction)
        s         = stats[key]

        nb_rb   = int(row[COL["nb_rb"]])
        mcs     = int(row[COL["mcs"]])
        tbs     = int(row[COL["tbs_bytes"]])
        sdu     = int(row[COL["sdu_bytes"]])
        cqi     = int(row[COL["cqi"]])
        retx    = int(row[COL["retx"]])
        harq    = int(row[COL["harq_pid"]])
        rb_util = float(row[COL["rb_util"]])

        # Update gauges
        s.cqi       = cqi
        s.mcs       = mcs
        s.nb_rb     = nb_rb
        s.rb_util   = rb_util
        s.tbs       = tbs
        s.sdu       = sdu
        s.harq_pid  = harq
        s.retx_round= retx

        # Update counters (always increasing)
        s.total_tbs  += tbs
        s.total_tx   += 1
        if retx > 0:
            s.total_retx += 1

    except (ValueError, IndexError) as e:
        print(f"[pusher] Skipping malformed row: {row} ({e})")

# ── CSV tail logic ───────────────────────────────────────────────────────────

def delete_old_metrics():
    """Wipe all metrics for this member from Pushgateway before starting a new session."""
    url = f"{PUSHGATEWAY_URL}/metrics/job/lte_mac/member/{MEMBER}"
    try:
        r = requests.delete(url, timeout=5)
        print(f"[pusher] Cleared old metrics (status {r.status_code})")
    except requests.exceptions.RequestException as e:
        print(f"[pusher] Could not clear old metrics: {e}")

def tail_csv():
    """
    Opens the CSV file and yields new rows as they are appended.
    Handles file recreation (new simulation run) by detecting inode changes.
    """
    # Wait for the file to exist
    while running:
        if os.path.exists(CSV_FILE):
            break
        print(f"[pusher] Waiting for {CSV_FILE} to appear...")
        time.sleep(2)

    print(f"[pusher] Found {CSV_FILE}, starting tail")

    delete_old_metrics()
    stats.clear()

    f = open(CSV_FILE, "r", newline="")
    reader = csv.reader(f)

    # Skip the header row
    try:
        header = next(reader)
        print(f"[pusher] Header: {header}")
    except StopIteration:
        pass

    current_inode = os.stat(CSV_FILE).st_ino
    last_push     = time.time()
    last_row_time  = time.time()
    INACTIVITY_TIMEOUT = 10
    metrics_cleared = False

    while running:
        # Check if the file was recreated (new simulation run)
        try:
            new_inode = os.stat(CSV_FILE).st_ino
        except FileNotFoundError:
            new_inode = None

        if new_inode != current_inode:
            print("[pusher] CSV file recreated, resetting stats")
            f.close()
            delete_old_metrics()
            stats.clear()
            time.sleep(1)
            f = open(CSV_FILE, "r", newline="")
            reader = csv.reader(f)
            try:
                next(reader)  # skip new header
            except StopIteration:
                pass
            current_inode = os.stat(CSV_FILE).st_ino
            last_push = time.time()
            continue

        # Read all available new lines
        rows_read = 0
        for row in reader:
            if row:
                process_row(row)
                rows_read += 1

        # Track last activity
        if rows_read > 0:
            last_row_time   = time.time()
            metrics_cleared = False

        # Push on interval
        now = time.time()
        if now - last_push >= PUSH_INTERVAL:
            if rows_read > 0 or stats:
                push()
                if rows_read > 0:
                    print(f"[pusher] Pushed {rows_read} new rows "
                          f"({len(stats)} UEs tracked)")
            last_push = now

        # Inactivity check — simulation stopped
        if not metrics_cleared and (time.time() - last_row_time) > INACTIVITY_TIMEOUT:
            print("[pusher] No new data for 10s — simulation stopped")
            print("[pusher] Clearing Pushgateway (Prometheus history preserved)")
            delete_old_metrics()
            stats.clear()
            metrics_cleared = True

        time.sleep(0.1)

    f.close()
    # Final push on shutdown
    # push()
    print("[pusher] Final push sent. Bye.")

# ── Entry point ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"[pusher] Starting")
    print(f"[pusher]   CSV file:       {CSV_FILE}")
    print(f"[pusher]   Pushgateway:    {PUSHGATEWAY_URL}")
    print(f"[pusher]   Member:         {MEMBER}")
    print(f"[pusher]   Push interval:  {PUSH_INTERVAL}s")
    tail_csv()
