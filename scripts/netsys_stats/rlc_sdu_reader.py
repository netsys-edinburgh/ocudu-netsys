#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Reads the per-SDU RLC AM statistics saved by stats_consumer.py, prints a summary and optionally exports a CSV file.

DL (the gNB is the RLC transmitter): number of SDUs per outcome and, for the SDUs that were ACKed after RLC reTxs
(result=ack), the distribution of the number of reTxs and of the time from the first transmission to the STATUS report
that ACKs them.

UL (the gNB is the RLC receiver): number of recovered SDUs and the time from the generation of the first STATUS report
that NACKs them to their full reception. The UE does not report its reTxs, so their number is unknown.

If the .json file of a run is present, its lost batches and dropped records are also reported, since they mean that
the data is incomplete.

Usage:
    pip install numpy
    python3 rlc_sdu_reader.py /data/rlc_dump/rlc_sdu_20261125_101500.bin
    python3 rlc_sdu_reader.py /data/rlc_dump --csv rlc_sdu.csv
"""

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

import numpy as np

import rlc_sdu_format as rsf
import stats_format

#: Columns of the exported CSV file, in order. Unknown values are left empty.
CSV_COLUMNS = (
    "wall_time_ns",
    "dir",
    "ue_index",
    "rb",
    "sn",
    "result",
    "nof_retxs",
    "max_retx_thresh",
    "sdu_len",
    "t_us",
)


def print_bearers(records: np.ndarray) -> None:
    """Prints the number of records per UE and radio bearer."""
    counts = Counter(zip(records["ue_index"].tolist(), records["rb_type"].tolist(), records["rb_id"].tolist()))
    for (ue_index, rb_type, rb_id), count in sorted(counts.items()):
        print(f"    ue={ue_index} {rsf.rb_name(rb_type, rb_id)}: {count}")


def print_summary(records: np.ndarray) -> None:
    if records.size == 0:
        print("No records.")
        return

    print(
        f"\n{records.size} records from {stats_format.format_time(int(records['wall_time_ns'].min()))} "
        f"to {stats_format.format_time(int(records['wall_time_ns'].max()))}, "
        f"{np.unique(records['ue_index']).size} UE(s)"
    )

    dl = records[records["dir"] == 0]
    if dl.size:
        print(f"\n== DL: {dl.size} SDUs that needed an RLC reTx")
        for result_value in (0, 1):
            count = int(np.count_nonzero(dl["result"] == result_value))
            if count:
                print(f"  {rsf.RESULTS[result_value]:<10} {count:>8}  ({100 * count / dl.size:5.1f}%)")
        acked = dl[dl["result"] == 0]
        if acked.size:
            print(f"  ACKed SDUs (result=ack): {acked.size}")
            retxs, counts = np.unique(acked["nof_retxs"], return_counts=True)
            for value, count in zip(retxs, counts):
                print(f"    nof_retxs={value}: {count:>8}  ({100 * count / acked.size:5.1f}%)")
            stats_format.print_time_stats("first tx -> ACK", acked["t_us"])
        print("  Per bearer:")
        print_bearers(dl)

    ul = records[records["dir"] == 1]
    if ul.size:
        print(f"\n== UL: {ul.size} SDUs recovered after a NACK")
        stats_format.print_time_stats("first NACK -> full reception", ul["t_us"])
        print("  Per bearer:")
        print_bearers(ul)


def known(value: int):
    """Returns the value, or an empty CSV cell if it is unknown."""
    return "" if value == rsf.UNKNOWN else value


def export_csv(records: np.ndarray, csv_path: Path) -> None:
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(CSV_COLUMNS)
        for rec in records.tolist():
            fields = dict(zip(rsf.RECORD_FIELDS, rec))
            writer.writerow(
                (
                    fields["wall_time_ns"],
                    rsf.DIRECTIONS.get(fields["dir"], fields["dir"]),
                    fields["ue_index"],
                    rsf.rb_name(fields["rb_type"], fields["rb_id"]),
                    fields["sn"],
                    rsf.RESULTS.get(fields["result"], fields["result"]),
                    known(fields["nof_retxs"]),
                    known(fields["max_retx_thresh"]),
                    fields["sdu_len"],
                    fields["t_us"],
                )
            )
    print(f"\nWrote {records.size} records to {csv_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path, help=".bin files, or directories containing them")
    parser.add_argument("--csv", type=Path, default=None, help="Export all the records to this CSV file")
    args = parser.parse_args()

    bin_files = stats_format.find_bin_files(args.paths, rsf)
    if not bin_files:
        print("No rlc_sdu .bin files found.", file=sys.stderr)
        return 1

    all_records = []
    for bin_path in bin_files:
        stats_format.report_run_info(bin_path)
        all_records.append(stats_format.load_records(bin_path, rsf))
    records = np.concatenate(all_records)

    print_summary(records)
    if args.csv:
        export_csv(records, args.csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
