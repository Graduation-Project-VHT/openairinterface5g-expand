#!/usr/bin/env python3
"""
ue_launcher.py — Sequential UE Launcher with Attachment Detection
==================================================================
    When a UE completes the LTE attach procedure, the SPGW assigns it
    an IP and the UE software creates a TUN interface 'oaitun_ue1'
    inside the container. We poll that interface once per second until
    it has a valid inet address.

Usage:
    uv run ue_launcher.py --ues 10
    uv run ue_launcher.py --ues 5 --timeout 45
    uv run ue_launcher.py --ues 10 --skip-failed
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import config


# ─── Docker helpers ───────────────────────────────────────────────────────────

def run_cmd(cmd: list[str], capture: bool = True) -> subprocess.CompletedProcess:
    """
    Run a shell command.

    capture=True  → stdout/stderr are captured and returned in result
    capture=False → output streams directly to your terminal in real time
                    (useful for docker compose, which prints progress lines)
    """
    return subprocess.run(cmd, capture_output=capture, text=True)


def container_is_running(container: str) -> bool:
    """
    Ask Docker whether a named container is currently in Running state.

    'docker inspect -f {{.State.Running}} <name>' prints either 'true' or 'false'.
    This is more reliable than checking docker ps output with grep.
    """
    result = run_cmd(
        ["docker", "inspect", "-f", "{{.State.Running}}", container]
    )
    return result.stdout.strip() == "true"


def get_tunnel_ip(container: str) -> str | None:
    """
    Return the IPv4 address on 'oaitun_ue1', or None if not up yet.

    We query the exact interface name 'oaitun_ue1' — NOT a substring like
    'oaitun_ue' — because 'oaitun_uem2' (emergency bearer) always exists
    with a fixed IP even before the UE has attached. Substring matching
    would give a false positive.

    Successful attach → 'ip addr show oaitun_ue1' returns something like:
        4: oaitun_ue1: <POINTOPOINT,UP,...>
            inet 12.0.0.2/24 scope global oaitun_ue1
                                    ↑ we want this
    """
    result = run_cmd(
        ["docker", "exec", container, "ip", "addr", "show", "oaitun_ue1"]
    )
    if result.returncode != 0:
        return None  # interface doesn't exist yet — UE not attached

    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("inet ") and not line.startswith("inet6"):
            # "inet 12.0.0.2/24 scope global oaitun_ue1"
            #       ^^^^^^^^^ split on space, then strip the /24 prefix length
            return line.split()[1].split("/")[0]

    return None  # interface exists but has no address yet


# ─── Per-UE lifecycle ─────────────────────────────────────────────────────────

def start_ue_service(index: int) -> bool:
    """
    Bring up a single UE service via docker compose.

    Why --no-recreate?
        Without it, 'docker compose up' would stop and recreate any already-
        running containers. That would disconnect UEs that are already attached.
        With --no-recreate: running = leave it, stopped = start it, missing = create it.
    """
    compose_base = Path(config.COMPOSE_DIR) / "docker-compose.yml"
    compose_ues  = Path(config.COMPOSE_DIR) / "docker-compose.ues.yml"
    service_name = f"oai_ue{index}"

    print(f"    → docker compose up -d {service_name}")
    result = run_cmd(
        [
            "docker", "compose",
            "-f", str(compose_base),
            "-f", str(compose_ues),
            "up", "-d", "--no-recreate",
            service_name,
        ],
        capture=False,  # stream docker compose output to terminal
    )
    return result.returncode == 0


def wait_for_attachment(container: str, index: int, timeout: int) -> str | None:
    """
    Poll once per second until oaitun_ue1 has a valid IP, or until timeout.

    Why poll instead of a fixed sleep(5)?
        Fixed sleep is unreliable:
          - Too short → race condition, we move on before UE is ready
          - Too long   → wasted time across 10 UEs adds up fast
        Polling exits the moment attachment succeeds — no sooner, no later.

    Returns the assigned IP on success, None on timeout.
    """
    expected_ip = config.ue_tunnel_ip(index)
    deadline    = time.monotonic() + timeout

    print(f"    Waiting for attachment (expected: {expected_ip}) ", end="", flush=True)

    while time.monotonic() < deadline:
        ip = get_tunnel_ip(container)
        if ip:
            print(f" ✓  got {ip}")
            return ip
        print(".", end="", flush=True)
        time.sleep(1)

    print(f"\n    ✗ Timed out after {timeout}s — container may not have attached")
    return None


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Start OAI LTE UE containers one by one with attachment detection",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  uv run ue_launcher.py --ues 10
  uv run ue_launcher.py --ues 5 --timeout 45
  uv run ue_launcher.py --ues 10 --skip-failed
        """,
    )
    parser.add_argument(
        "--ues", type=int, required=True,
        help=f"Number of UEs to launch (1–{config.MAX_UES})"
    )
    parser.add_argument(
        "--timeout", type=int, default=30,
        help="Max seconds to wait per UE before giving up (default: 30)"
    )
    parser.add_argument(
        "--skip-failed", action="store_true",
        help="Continue launching remaining UEs even if one fails to attach "
             "(by default, the script stops at the first failure)"
    )
    args = parser.parse_args()

    if not (1 <= args.ues <= config.MAX_UES):
        print(f"ERROR: --ues must be between 1 and {config.MAX_UES}.")
        sys.exit(1)

    print(f"""
╔══════════════════════════════════════════════════════════════╗
║  OAI 4G RFSim — Sequential UE Launcher                      ║
╠══════════════════════════════════════════════════════════════╣
║  UEs to launch  : {args.ues:<42} ║
║  Timeout / UE   : {str(args.timeout) + 's':<42} ║
║  Skip on fail   : {str(args.skip_failed):<42} ║
╚══════════════════════════════════════════════════════════════╝
""")

    succeeded: list[int] = []
    failed:    list[int] = []

    for i in range(args.ues):
        container = config.ue_container_name(i)

        print(f"\n[UE {i:>2}/{args.ues - 1}]  {container}")
        print(f"           Expected tunnel IP : {config.ue_tunnel_ip(i)}")
        print(f"           noise_power_dB     : {config.ue_noise_power_db(i)} dB")

        # ── Step 1: start the container ───────────────────────────────────────
        if not start_ue_service(i):
            print(f"    ✗ 'docker compose up' failed for UE {i}")
            failed.append(i)
            if not args.skip_failed:
                print("    Stopping. Use --skip-failed to continue past failures.")
                break
            continue

        # ── Step 2: wait for radio attachment ─────────────────────────────────
        ip = wait_for_attachment(container, i, args.timeout)

        if not ip:
            failed.append(i)
            if not args.skip_failed:
                print("    Stopping. Use --skip-failed to continue past failures.")
                break
            continue

        succeeded.append(i)

    # ─── Summary ──────────────────────────────────────────────────────────────
    print(f"""
╔══════════════════════════════════════════════════════════════╗
║  Launch summary                                              ║
╠══════════════════════════════════════════════════════════════╣
║  Requested : {args.ues:<47} ║
║  Succeeded : {len(succeeded):<47} ║
║  Failed    : {len(failed):<47} ║""")

    if failed:
        failed_str = str(failed)
        print(f"║  Failed UEs: {failed_str:<47} ║")

    print("╚══════════════════════════════════════════════════════════════╝")

    if succeeded:
        n = len(succeeded)
        if args.start_traffic:
            # Build the traffic_gen.py command, forwarding only the args
            # that were explicitly given — omitted ones use traffic_gen's
            # own defaults (config.DEFAULT_BANDWIDTH, config.DEFAULT_DURATION).
            traffic_cmd = [sys.executable, "traffic_gen.py", "--ues", str(n)]
            if args.bandwidth:
                traffic_cmd += ["--bandwidth", args.bandwidth]
            if args.duration is not None:
                traffic_cmd += ["--duration", str(args.duration)]
            if args.label:
                traffic_cmd += ["--label", args.label]
            if args.no_upload:
                traffic_cmd += ["--no-upload"]

            print(f"\n  All {n} UEs attached. Starting traffic immediately...")
            print(f"  → {' '.join(traffic_cmd)}\n")
            result = run_cmd(traffic_cmd, capture=False)
            sys.exit(result.returncode)
        else:
            print(f"""
    All attached UEs are ready. Next step:
    uv run traffic_gen.py --ues {n}
""")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
