#!/usr/bin/env python3
"""
Aggregate repeated SHM CSV runs.

Input files are expected to be output/newshm*.csv with the same schema as
analysis_summary.csv. Rows are matched by:

  ipc, n_msgs, n_cons, target_mps, consumer_id

Rows with p50_ns above the configured threshold are dropped entirely, then the
remaining rows for each matched group are averaged column-by-column and written
to output/shm_final.csv.
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path


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

KEY_FIELDS = ["ipc", "n_msgs", "n_cons", "target_mps", "consumer_id"]
INT_FIELDS = {"n_msgs", "n_cons", "target_mps", "consumer_id"}


def parse_float(value: str) -> float:
    if value is None or value == "":
        return math.nan
    return float(value)


def row_key(row):
    return tuple(row[field] for field in KEY_FIELDS)


def sort_key(row):
    return (
        row["ipc"],
        int(float(row["n_msgs"])),
        int(float(row["n_cons"])),
        int(float(row["target_mps"])),
        int(float(row["consumer_id"])),
    )


def read_rows(paths):
    rows = []
    for path in paths:
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            missing = [field for field in CSV_FIELDS if field not in (reader.fieldnames or [])]
            if missing:
                raise ValueError(f"{path} missing columns: {', '.join(missing)}")
            for row in reader:
                row = {field: row.get(field, "") for field in CSV_FIELDS}
                row["_source_file"] = path.name
                rows.append(row)
    return rows


def average_group(rows):
    out = {}
    first = rows[0]
    for field in CSV_FIELDS:
        if field == "ipc":
            out[field] = first[field]
        elif field in INT_FIELDS:
            out[field] = str(int(float(first[field])))
        else:
            values = [parse_float(row[field]) for row in rows]
            values = [v for v in values if math.isfinite(v)]
            out[field] = f"{(sum(values) / len(values)):.12g}" if values else ""
    return out


def aggregate_shm_csvs(input_paths, output_path: Path, p50_threshold_ns=1000.0):
    rows = read_rows(input_paths)
    groups = defaultdict(list)
    for row in rows:
        if row["ipc"] != "shm":
            continue
        groups[row_key(row)].append(row)

    final_rows = []
    dropped_rows = []
    for key in sorted(groups):
        group = groups[key]
        kept = []
        for row in group:
            p50 = parse_float(row["p50_ns"])
            if math.isfinite(p50) and p50 > p50_threshold_ns:
                dropped_rows.append(row)
            else:
                kept.append(row)

        if not kept:
            # Last-resort guard: keep the lowest-p50 row so the configuration
            # remains present even if every run exceeded the threshold.
            kept = [min(group, key=lambda row: parse_float(row["p50_ns"]))]

        final_rows.append(average_group(kept))

    final_rows.sort(key=sort_key)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(final_rows)

    return final_rows, dropped_rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, default=Path("output"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--p50-threshold-ns", type=float, default=1000.0)
    args = parser.parse_args()

    input_paths = sorted(args.out_dir.glob("newshm*.csv"))
    if not input_paths:
        raise SystemExit(f"No newshm*.csv files found in {args.out_dir}")

    output_path = args.output or (args.out_dir / "shm_final.csv")
    final_rows, dropped_rows = aggregate_shm_csvs(
        input_paths,
        output_path,
        p50_threshold_ns=args.p50_threshold_ns,
    )
    print(f"Read {len(input_paths)} SHM CSV files")
    print(f"Wrote {len(final_rows)} averaged rows to {output_path}")
    print(f"Dropped {len(dropped_rows)} rows with p50_ns > {args.p50_threshold_ns:g}")


if __name__ == "__main__":
    main()
