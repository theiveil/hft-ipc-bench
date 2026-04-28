#!/usr/bin/env python3
"""
Offline analysis + visualisation for hft-ipc-bench.

File naming convention (all in one flat directory):
  bench_{ipc}_N{n}_C{c}_tsc_ghz.txt
  bench_{ipc}_N{n}_C{c}_s1.ns
  latency_{ipc}_N{n}_C{c}_c{id}.bin
  bench_{ipc}_N{n}_C{c}_c{id}.tp

Modes
-----
  # Legacy: reads /tmp/ with old un-tagged names (no matplotlib)
  python3 analyze.py [shm|zmq|grpc|redis|all]

  # Sweep mode: called by run_bench.sh
  python3 analyze.py --out-dir   ./output        \
                     --plots-dir ./output/plots   \
                     --csv-out   ./output/analysis_summary.csv \
                     --ipc-list  shm zmq grpc redis \
                     --msg-list  100000 1000000 10000000 \
                     --cons-list 1 2 4
"""

import argparse
import csv
import contextlib
import math
import os
import struct
import sys
from pathlib import Path

# ── Optional imports ──────────────────────────────────────────────────────────
Path("/tmp/matplotlib").mkdir(parents=True, exist_ok=True)
os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.environ.setdefault("XDG_CACHE_HOME", "/tmp")


@contextlib.contextmanager
def suppress_stdio():
    with open(os.devnull, "w") as devnull:
        old_stdout = os.dup(1)
        old_stderr = os.dup(2)
        try:
            os.dup2(devnull.fileno(), 1)
            os.dup2(devnull.fileno(), 2)
            yield
        finally:
            os.dup2(old_stdout, 1)
            os.dup2(old_stderr, 2)
            os.close(old_stdout)
            os.close(old_stderr)

try:
    with suppress_stdio():
        import matplotlib
        matplotlib.use("Agg")
        matplotlib.set_loglevel("error")
        import matplotlib.pyplot as plt
        import matplotlib.ticker as mticker
        import numpy as np
    HAS_MPL = True
except ImportError:
    HAS_MPL = False

try:
    with suppress_stdio():
        import seaborn as sns
        sns.set_theme(style="darkgrid", palette="deep")
except ImportError:
    pass

# ── Palette ───────────────────────────────────────────────────────────────────
IPC_COLOR = {"shm": "#2196F3", "zmq": "#4CAF50", "grpc": "#FF9800", "redis": "#F44336"}
IPC_LABEL = {
    "shm":   "SHM (lock-free)",
    "zmq":   "ZeroMQ PUB/SUB",
    "grpc":  "gRPC streaming",
    "redis": "Redis Pub/Sub",
}
SUPPORTED = ["shm", "zmq", "grpc", "redis"]
CSV_FIELDS = [
    "ipc",
    "n_msgs",
    "n_cons",
    "target_mps",
    "consumer_id",
    "tsc_ghz",
    "n_samples",
    "p50_ns",
    "p90_ns",
    "p99_ns",
    "p999_ns",
    "max_ns",
    "avg_jitter_ns",
    "max_jitter_ns",
    "elapsed_s",
    "throughput_mps",
    "n_recv",
    "n_exp",
    "loss",
]


# ════════════════════════════════════════════════════════════════════════════
# Statistics (stdlib)
# ════════════════════════════════════════════════════════════════════════════

def percentile(sd: list, pct: float) -> float:
    n = len(sd)
    if n == 0:
        return 0.0
    idx  = pct / 100.0 * (n - 1)
    lo   = int(idx)
    frac = idx - lo
    if lo + 1 >= n:
        return float(sd[-1])
    return sd[lo] * (1.0 - frac) + sd[lo + 1] * frac


def mean(data: list) -> float:
    return sum(data) / len(data) if data else 0.0


# ════════════════════════════════════════════════════════════════════════════
# I/O
# ════════════════════════════════════════════════════════════════════════════

def _tsc_path(base: Path, ipc: str, n_tag: str) -> Path:
    return base / f"bench_{ipc}{n_tag}_tsc_ghz.txt"

def _s1_path(base: Path, ipc: str, n_tag: str) -> Path:
    return base / f"bench_{ipc}{n_tag}_s1.ns"

def _lat_path(base: Path, ipc: str, n_tag: str, cid: int) -> Path:
    return base / f"latency_{ipc}{n_tag}_c{cid}.bin"

def _tp_path(base: Path, ipc: str, n_tag: str, cid: int) -> Path:
    return base / f"bench_{ipc}{n_tag}_c{cid}.tp"


def read_tsc_ghz(base: Path, ipc: str, n_tag: str = "") -> float:
    path = _tsc_path(base, ipc, n_tag)
    try:
        return float(path.read_text().strip())
    except FileNotFoundError:
        pass
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if "cpu MHz" in line:
                return float(line.split(":")[1].strip()) / 1000.0
    except Exception:
        pass
    return 3.0


def read_latency_ns(lat_file: Path, tsc_ghz: float) -> list:
    raw = lat_file.read_bytes()
    n   = len(raw) // 8
    return [c / tsc_ghz for c in struct.unpack_from(f"<{n}Q", raw)]


def read_throughput(tp_file: Path, s1_file: Path):
    if not tp_file.exists() or not s1_file.exists():
        return None
    s1_ns  = int(s1_file.read_text().strip())
    lines  = tp_file.read_text().strip().splitlines()
    s2_ns  = int(lines[0])
    n_recv = int(lines[1])
    n_exp  = int(lines[2])
    elapsed = (s2_ns - s1_ns) / 1e9
    mps     = n_recv / elapsed if elapsed > 0 else 0.0
    return mps, elapsed, n_recv, n_exp


def write_csv(csv_file: Path, rows: list) -> None:
    csv_file.parent.mkdir(parents=True, exist_ok=True)
    with csv_file.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


# ════════════════════════════════════════════════════════════════════════════
# Analysis report — returns plot data plus CSV rows
# ════════════════════════════════════════════════════════════════════════════

def text_report(base: Path, ipc: str, n_tag: str,
                n_msgs=None, n_cons=None, target_mps=None) -> tuple:
    tsc_ghz = read_tsc_ghz(base, ipc, n_tag)

    s1_file = _s1_path(base, ipc, n_tag)
    results = {}
    rows = []
    cid = 0
    while True:
        lat_file = _lat_path(base, ipc, n_tag, cid)
        tp_file  = _tp_path(base, ipc, n_tag, cid)
        if not lat_file.exists():
            break

        ns        = read_latency_ns(lat_file, tsc_ghz)
        if not ns:
            cid += 1
            continue
        ns_sorted = sorted(ns)
        jitter    = [abs(ns[i] - ns[i-1]) for i in range(1, len(ns))]
        tp        = read_throughput(tp_file, s1_file)

        p50  = percentile(ns_sorted, 50)
        p90  = percentile(ns_sorted, 90)
        p99  = percentile(ns_sorted, 99)
        p999 = percentile(ns_sorted, 99.9)
        max_ns = max(ns)
        avg_jitter = mean(jitter)
        max_jitter = max(jitter) if jitter else 0.0
        mps  = tp[0] if tp else 0.0
        elapsed = None
        n_recv = None
        n_exp = None
        loss = None
        if tp:
            mps, elapsed, n_recv, n_exp = tp
            loss = n_exp - n_recv

        results[cid] = dict(ns=ns, ns_sorted=ns_sorted,
                            p50=p50, p90=p90, p99=p99, p999=p999,
                            mps=mps)
        rows.append({
            "ipc": ipc,
            "n_msgs": n_msgs,
            "n_cons": n_cons,
            "target_mps": target_mps,
            "consumer_id": cid,
            "tsc_ghz": tsc_ghz,
            "n_samples": len(ns),
            "p50_ns": p50,
            "p90_ns": p90,
            "p99_ns": p99,
            "p999_ns": p999,
            "max_ns": max_ns,
            "avg_jitter_ns": avg_jitter,
            "max_jitter_ns": max_jitter,
            "elapsed_s": elapsed,
            "throughput_mps": mps,
            "n_recv": n_recv,
            "n_exp": n_exp,
            "loss": loss,
        })
        cid += 1

    return results, rows


# ════════════════════════════════════════════════════════════════════════════
# Plot 1 — CDF  (one figure per N×C, one curve per IPC)
# ════════════════════════════════════════════════════════════════════════════

def plot_cdf(combo: dict, n_msgs: int, n_cons: int, out: Path):
    """combo: {ipc: {cid: {ns_sorted, ...}}}"""
    if not HAS_MPL:
        return
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.set_title(f"Latency CDF — N={n_msgs:,}  C={n_cons}",
                 fontsize=14, fontweight="bold")

    for ipc in SUPPORTED:
        d = combo.get(ipc, {}).get(0)
        if not d:
            continue
        sd = d["ns_sorted"]
        n  = len(sd)
        if n == 0:
            continue
        step = max(1, n // 5000)
        xs = sd[::step]
        ys = [(i + 1) / n for i in range(0, n, step)]
        if xs[-1] != sd[-1]:
            xs.append(sd[-1]); ys.append(1.0)
        ax.plot(xs, ys, label=IPC_LABEL[ipc], color=IPC_COLOR[ipc], linewidth=2)

    ax.set_xlabel("Latency (ns)", fontsize=12)
    ax.set_ylabel("Cumulative Probability", fontsize=12)
    ax.set_xscale("log")
    ax.yaxis.set_major_formatter(mticker.PercentFormatter(xmax=1.0, decimals=0))
    for pct, label in [(0.50, "p50"), (0.99, "p99")]:
        ax.axhline(pct, color="grey", linestyle=":", linewidth=0.8, alpha=0.7)
        ax.text(ax.get_xlim()[0] or 1, pct + 0.002, f" {label}",
                color="grey", fontsize=8, va="bottom")
    ax.legend(fontsize=11)
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════════════════
# Plot 2 — Latency over time scatter  (one per IPC × N × C, consumer 0)
# ════════════════════════════════════════════════════════════════════════════

def plot_scatter(ns: list, ipc: str, n_msgs: int, n_cons: int, out: Path):
    if not HAS_MPL:
        return
    n    = len(ns)
    step = max(1, n // 8000)
    xs   = list(range(0, n, step))
    ys   = ns[::step]

    sd   = sorted(ys)
    p50  = percentile(sd, 50)
    p99  = percentile(sd, 99)

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.set_title(f"Latency over Time — {IPC_LABEL[ipc]}  N={n_msgs:,}  C={n_cons}",
                 fontsize=13, fontweight="bold")
    ax.scatter(xs, ys, s=1.5, alpha=0.35, color=IPC_COLOR[ipc], rasterized=True)
    ax.axhline(p50, color="lime",   linestyle="--", linewidth=1.2,
               label=f"p50 = {p50:.0f} ns")
    ax.axhline(p99, color="tomato", linestyle="--", linewidth=1.2,
               label=f"p99 = {p99:.0f} ns")
    ax.set_xlabel("Message index", fontsize=11)
    ax.set_ylabel("Latency (ns)",  fontsize=11)
    ax.set_yscale("log")
    ax.legend(fontsize=10)
    ax.grid(True, which="both", alpha=0.25)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


# ════════════════════════════════════════════════════════════════════════════
# Plot 3 — Radar  (MPS + latency axes, one per N×C)
# ════════════════════════════════════════════════════════════════════════════

def plot_radar(combo: dict, n_msgs: int, n_cons: int, out: Path):
    """combo: {ipc: {cid: {mps, p50, p99, p999}}}"""
    if not HAS_MPL:
        return

    metrics = {}
    for ipc in SUPPORTED:
        d = combo.get(ipc, {}).get(0)
        if d:
            metrics[ipc] = d
    if len(metrics) < 2:
        return

    axis_labels = [
        "MPS\n(↑ better)",
        "1/p50\n(↓ latency better)",
        "1/p99\n(↓ latency better)",
        "1/p99.9\n(↓ latency better)",
    ]
    N_AX = len(axis_labels)

    def raw(d):
        return [
            d["mps"],
            1.0 / max(d["p50"],  1e-9),
            1.0 / max(d["p99"],  1e-9),
            1.0 / max(d["p999"], 1e-9),
        ]

    all_raw  = [raw(v) for v in metrics.values()]
    ax_max   = [max(col) or 1.0 for col in zip(*all_raw)]
    angles   = [2 * math.pi * i / N_AX for i in range(N_AX)] + [0]

    fig, ax = plt.subplots(figsize=(8, 8), subplot_kw=dict(polar=True))
    ax.set_title(f"IPC Radar — N={n_msgs:,}  C={n_cons}",
                 fontsize=14, fontweight="bold", pad=22)

    for ipc, d in metrics.items():
        vals = [v / m for v, m in zip(raw(d), ax_max)] + [raw(d)[0] / ax_max[0]]
        ax.plot(angles, vals, color=IPC_COLOR[ipc], linewidth=2.5,
                label=IPC_LABEL[ipc])
        ax.fill(angles, vals, color=IPC_COLOR[ipc], alpha=0.12)

    ax.set_xticks(angles[:-1])
    ax.set_xticklabels(axis_labels, fontsize=10)
    ax.set_ylim(0, 1)
    ax.set_yticks([0.25, 0.50, 0.75, 1.0])
    ax.set_yticklabels(["25%", "50%", "75%", "100%"], fontsize=8)
    ax.legend(loc="upper right", bbox_to_anchor=(1.38, 1.18), fontsize=11)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ════════════════════════════════════════════════════════════════════════════
# Sweep entry point
# ════════════════════════════════════════════════════════════════════════════

def run_sweep(out_dir: Path, plots_dir: Path, ipc_list, msg_list, cons_list, mps_list,
              csv_out: Path):
    plots_dir.mkdir(parents=True, exist_ok=True)
    csv_rows = []

    for n_msgs in msg_list:
        for n_cons in cons_list:
            for mps in mps_list:
                n_tag   = f"_N{n_msgs}_C{n_cons}_MPS{mps}"
                combo   = {}

                for ipc in ipc_list:
                    # Check whether any file for this combo exists
                    if not _tsc_path(out_dir, ipc, n_tag).exists():
                        continue
                    combo[ipc], rows = text_report(out_dir, ipc, n_tag,
                                                   n_msgs, n_cons, mps)
                    csv_rows.extend(rows)

                if not combo:
                    continue

                tag = f"N{n_msgs}_C{n_cons}_MPS{mps}"

                # CDF
                plot_cdf(combo, n_msgs, n_cons,
                         plots_dir / f"cdf_{tag}.png")

                # Scatter per IPC
                for ipc, cdata in combo.items():
                    if 0 not in cdata:
                        continue
                    lat_file = _lat_path(out_dir, ipc, n_tag, 0)
                    if lat_file.exists():
                        tsc = read_tsc_ghz(out_dir, ipc, n_tag)
                        ns  = read_latency_ns(lat_file, tsc)
                        plot_scatter(ns, ipc, n_msgs, n_cons,
                                     plots_dir / f"scatter_{ipc}_{tag}.png")

                # Radar
                plot_radar(combo, n_msgs, n_cons,
                           plots_dir / f"radar_{tag}.png")

    write_csv(csv_out, csv_rows)


# ════════════════════════════════════════════════════════════════════════════
# Legacy single-run mode  (reads /tmp/, old un-tagged names)
# ════════════════════════════════════════════════════════════════════════════

def legacy_report(ipc: str) -> list:
    """Reads /tmp/bench_{ipc}_tsc_ghz.txt etc. (no N/C tag)."""
    base = Path("/tmp")
    tsc_ghz = read_tsc_ghz(base, ipc, "")   # n_tag=""  → old filenames

    s1_file = base / f"bench_{ipc}_s1.ns"
    cid = 0
    rows = []
    while True:
        lat_file = base / f"latency_{ipc}_c{cid}.bin"
        tp_file  = base / f"bench_{ipc}_c{cid}.tp"
        if not lat_file.exists():
            break
        ns        = read_latency_ns(lat_file, tsc_ghz)
        ns_sorted = sorted(ns)
        jitter    = [abs(ns[i] - ns[i-1]) for i in range(1, len(ns))]
        tp        = read_throughput(tp_file, s1_file)
        p50  = percentile(ns_sorted, 50)
        p90  = percentile(ns_sorted, 90)
        p99  = percentile(ns_sorted, 99)
        p999 = percentile(ns_sorted, 99.9)
        elapsed = None
        mps = 0.0
        n_recv = None
        n_exp = None
        loss = None
        if tp:
            mps, elapsed, n_recv, n_exp = tp
            loss = n_exp - n_recv
        rows.append({
            "ipc": ipc,
            "n_msgs": None,
            "n_cons": None,
            "target_mps": None,
            "consumer_id": cid,
            "tsc_ghz": tsc_ghz,
            "n_samples": len(ns),
            "p50_ns": p50,
            "p90_ns": p90,
            "p99_ns": p99,
            "p999_ns": p999,
            "max_ns": max(ns) if ns else 0.0,
            "avg_jitter_ns": mean(jitter),
            "max_jitter_ns": max(jitter) if jitter else 0.0,
            "elapsed_s": elapsed,
            "throughput_mps": mps,
            "n_recv": n_recv,
            "n_exp": n_exp,
            "loss": loss,
        })
        cid += 1

    return rows


# ════════════════════════════════════════════════════════════════════════════
# CLI
# ════════════════════════════════════════════════════════════════════════════

def main():
    if "--out-dir" in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument("--out-dir",    required=True, type=Path)
        parser.add_argument("--plots-dir",  required=True, type=Path)
        parser.add_argument("--ipc-list",   nargs="+", default=SUPPORTED)
        parser.add_argument("--msg-list",   nargs="+", type=int,
                            default=[100_000, 1_000_000, 10_000_000])
        parser.add_argument("--cons-list",  nargs="+", type=int,
                            default=[1, 2, 4])
        parser.add_argument("--mps-list",   nargs="+", type=int,
                            default=[2_000_000, 5_000_000, 10_000_000])
        parser.add_argument("--csv-out",    type=Path)
        args = parser.parse_args()
        csv_out = args.csv_out or (args.out_dir / "analysis_summary.csv")
        run_sweep(args.out_dir, args.plots_dir,
                  args.ipc_list, args.msg_list, args.cons_list, args.mps_list,
                  csv_out)
    else:
        targets = [a for a in sys.argv[1:] if a != "all"]
        if not targets:
            targets = SUPPORTED
        csv_rows = []
        for ipc in targets:
            if ipc not in SUPPORTED:
                continue
            csv_rows.extend(legacy_report(ipc))
        write_csv(Path("/tmp/hft_ipc_analysis_summary.csv"), csv_rows)


if __name__ == "__main__":
    main()
