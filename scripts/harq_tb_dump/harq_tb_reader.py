#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Reads the per-TB HARQ statistics saved by harq_tb_consumer.py, prints a summary and optionally exports a CSV file.

For each direction, the summary shows the number of TBs per outcome and, for the TBs that were recovered by HARQ
reTxs (result=ack), the distribution of the number of reTxs and of the time from the first transmission to the
HARQ-ACK/CRC of the successful reTx (t_ack_us). If the .json file of a run is present, its lost batches and dropped
records are also reported, since they mean that the data is incomplete.

Usage:
    pip install numpy
    python3 harq_tb_reader.py /data/harq_dump/harq_tb_20261124_153000.bin
    python3 harq_tb_reader.py /data/harq_dump --csv harq_tb.csv
"""

import argparse
import csv
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

import harq_tb_format as htf

#: Columns of the exported CSV file, in order.
CSV_COLUMNS = (
    "wall_time_ns",
    "dir",
    "ue_index",
    "rnti",
    "h_id",
    "result",
    "nof_retxs",
    "max_retxs",
    "tbs_bytes",
    "first_tx_sfn",
    "first_tx_slot",
    "ack_sfn",
    "ack_slot",
    "t_ack_us",
)


def find_bin_files(paths):
    """Expands directories into the .bin files they contain."""
    files = []
    for path in paths:
        if path.is_dir():
            files.extend(sorted(path.glob("harq_tb_*.bin")))
        else:
            files.append(path)
    return files


def load_records(bin_path: Path) -> np.ndarray:
    """Loads the records of one .bin file. A truncated last record, e.g. from a killed consumer, is ignored."""
    size = bin_path.stat().st_size
    if size % htf.RECORD_SIZE:
        print(f"WARNING: {bin_path} ends with {size % htf.RECORD_SIZE} bytes of a truncated record", file=sys.stderr)
    return np.fromfile(bin_path, dtype=htf.record_dtype(), count=size // htf.RECORD_SIZE)


def report_run_info(bin_path: Path) -> None:
    """Prints the warnings found in the .json file of a run, if present."""
    json_path = bin_path.with_suffix(".json")
    if not json_path.exists():
        print(f"{bin_path.name}: no .json file found, completeness unknown")
        return
    info = json.loads(json_path.read_text())
    sessions = info.get("gnb_sessions", [])
    dropped_before = sessions[0]["first_nof_dropped_total"] if sessions else 0
    print(
        f"{bin_path.name}: {info['start_time']} to {info['end_time'] or 'not stopped cleanly'}, "
        f"{info['nof_records']} records, {len(sessions)} gNB session(s)"
    )
    if info["nof_lost_batches"]:
        print(f"  WARNING: {info['nof_lost_batches']} batch(es) lost in transit")
    if info.get("nof_dropped_during_run"):
        print(f"  WARNING: {info['nof_dropped_during_run']} record(s) dropped by the gNB while recording")
    if dropped_before:
        print(f"  Note: {dropped_before} record(s) dropped by the gNB before the consumer connected")
    if info["nof_malformed_messages"]:
        print(f"  WARNING: {info['nof_malformed_messages']} malformed message(s) skipped")


def format_time(wall_time_ns: int) -> str:
    return datetime.fromtimestamp(wall_time_ns / 1e9, tz=timezone.utc).isoformat(timespec="milliseconds")


def print_summary(records: np.ndarray) -> None:
    if records.size == 0:
        print("No records.")
        return

    print(
        f"\n{records.size} records from {format_time(int(records['wall_time_ns'].min()))} "
        f"to {format_time(int(records['wall_time_ns'].max()))}, {np.unique(records['rnti']).size} RNTI(s)"
    )

    for dir_value, dir_name in htf.DIRECTIONS.items():
        dir_records = records[records["dir"] == dir_value]
        if dir_records.size == 0:
            continue
        print(f"\n== {dir_name}: {dir_records.size} TBs that needed a reTx or were dropped")
        for result_value, result_name in htf.RESULTS.items():
            count = int(np.count_nonzero(dir_records["result"] == result_value))
            if count:
                print(f"  {result_name:<17} {count:>8}  ({100 * count / dir_records.size:5.1f}%)")

        acked = dir_records[dir_records["result"] == 0]
        if acked.size == 0:
            continue
        print(f"  Recovered TBs (result=ack): {acked.size}")
        retxs, counts = np.unique(acked["nof_retxs"], return_counts=True)
        for value, count in zip(retxs, counts):
            print(f"    nof_retxs={value}: {count:>8}  ({100 * count / acked.size:5.1f}%)")
        t_ack_ms = acked["t_ack_us"] / 1000.0
        p50, p90, p99 = np.percentile(t_ack_ms, [50, 90, 99])
        print(
            f"    t_ack: mean={t_ack_ms.mean():.2f} ms  p50={p50:.2f} ms  p90={p90:.2f} ms  "
            f"p99={p99:.2f} ms  max={t_ack_ms.max():.2f} ms"
        )


def export_csv(records: np.ndarray, csv_path: Path) -> None:
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(CSV_COLUMNS)
        for rec in records.tolist():
            fields = dict(zip(htf.RECORD_FIELDS, rec))
            writer.writerow(
                (
                    fields["wall_time_ns"],
                    htf.DIRECTIONS.get(fields["dir"], fields["dir"]),
                    fields["ue_index"],
                    f"{fields['rnti']:#06x}",
                    fields["h_id"],
                    htf.RESULTS.get(fields["result"], fields["result"]),
                    fields["nof_retxs"],
                    fields["max_retxs"],
                    fields["tbs_bytes"],
                    *htf.slot_to_sfn_slot(fields["first_tx_count"], fields["numerology"]),
                    *htf.slot_to_sfn_slot(fields["ack_count"], fields["numerology"]),
                    fields["t_ack_us"],
                )
            )
    print(f"\nWrote {records.size} records to {csv_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path, help=".bin files, or directories containing them")
    parser.add_argument("--csv", type=Path, default=None, help="Export all the records to this CSV file")
    args = parser.parse_args()

    bin_files = find_bin_files(args.paths)
    if not bin_files:
        print("No .bin files found.", file=sys.stderr)
        return 1

    all_records = []
    for bin_path in bin_files:
        report_run_info(bin_path)
        all_records.append(load_records(bin_path))
    records = np.concatenate(all_records)

    print_summary(records)
    if args.csv:
        export_csv(records, args.csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
