#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Reference consumer for the phy_tap SRS IQ dump ZMQ stream.

Connects to the ZeroMQ PUSH socket bound by the gNB (via the `srs_iq_dump=tcp://*:PORT` phy_tap_arguments option),
and for every SRS occasion received, saves the IQ samples as a `.npy` file (shape: [nof_symbols, nof_ports,
nof_subc], dtype complex64) alongside a `.json` sidecar with the occasion's metadata.

Usage:
    pip install pyzmq numpy
    python3 srs_iq_dump_consumer.py tcp://127.0.0.1:5556 --out-dir /tmp/srs_dump
"""

import argparse
import json
import struct
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
import zmq

# Must be kept in sync with `srs_iq_dump_header` in
# phy_tap_plugin_example/lib/external_processors/srs_iq_dump_zmq.h
_HEADER_FORMAT = "<IHHIHHBBBHBBBHBBBH"
_HEADER_FIELDS = [
    "magic",
    "version",
    "rnti",
    "sfn",
    "slot_index",
    "scs_khz",
    "start_symbol",
    "nof_symbols",
    "nof_ports",
    "nof_subc",
    "comb_size",
    "comb_offset",
    "cyclic_shift",
    "sequence_id",
    "configuration_index",
    "bandwidth_index",
    "freq_position",
    "freq_shift",
]
_HEADER_MAGIC = 0x53525349  # "SRSI"
_HEADER_SIZE = struct.calcsize(_HEADER_FORMAT)
assert _HEADER_SIZE == 31, f"Header format/size mismatch: {_HEADER_SIZE} (expected 31)"


def parse_header(raw: bytes) -> dict:
    if len(raw) != _HEADER_SIZE:
        raise ValueError(f"Unexpected header size: {len(raw)} bytes (expected {_HEADER_SIZE})")
    values = struct.unpack(_HEADER_FORMAT, raw)
    header = dict(zip(_HEADER_FIELDS, values))
    if header["magic"] != _HEADER_MAGIC:
        raise ValueError(f"Bad magic number: {header['magic']:#x} (expected {_HEADER_MAGIC:#x})")
    return header


def format_status_lines(rnti_counts: dict, count: int, last_summary: str) -> list:
    """Builds the status block: an aggregate line on top, followed by one line per RNTI seen so far."""
    lines = [f"RNTIs seen: {len(rnti_counts)} | Occasions captured: {count} | Last: {last_summary}"]
    for rnti in sorted(rnti_counts):
        lines.append(f"  rnti={rnti:#06x}: {rnti_counts[rnti]} occasion(s)")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", help="ZeroMQ address to connect to, e.g. tcp://127.0.0.1:5556")
    parser.add_argument("--out-dir", default=".", help="Directory to save the .npy/.json occasion files into")
    parser.add_argument("--limit", type=int, default=None, help="Stop after this many occasions (default: unlimited)")
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Also print a scrolling detail line for every occasion, in addition to the live counters",
    )
    args = parser.parse_args()
    live_status = sys.stderr.isatty() and not args.verbose

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    context = zmq.Context()
    socket = context.socket(zmq.PULL)
    socket.connect(args.address)
    print(f"Connected to {args.address}, saving occasions to {out_dir}. Press Ctrl+C to stop.", file=sys.stderr)

    count = 0
    rnti_counts = {}
    prev_nof_lines = 0
    try:
        while args.limit is None or count < args.limit:
            # recv_multipart() blocks. A Ctrl+C while blocked can surface either as a KeyboardInterrupt or as a
            # ZMQError(EINTR), depending on exactly when the signal lands relative to the underlying poll/recv call -
            # handle both the same way, as a clean request to stop.
            try:
                header_bytes, payload_bytes = socket.recv_multipart()
            except zmq.error.ZMQError as exc:
                if exc.errno == zmq.EINTR:
                    break
                raise

            try:
                header = parse_header(header_bytes)
            except ValueError as exc:
                print(f"Skipping malformed message: {exc}", file=sys.stderr)
                continue

            shape = (header["nof_symbols"], header["nof_ports"], header["nof_subc"])
            expected_bytes = shape[0] * shape[1] * shape[2] * np.dtype(np.complex64).itemsize
            if len(payload_bytes) != expected_bytes:
                print(
                    f"Skipping occasion with mismatched payload size: got {len(payload_bytes)} bytes, "
                    f"expected {expected_bytes} for shape {shape}",
                    file=sys.stderr,
                )
                continue

            iq = np.frombuffer(payload_bytes, dtype=np.complex64).reshape(shape)

            # Timestamp is wall-clock time on the consumer, i.e. when the occasion was received here - not a
            # field from the gNB itself (the header's sfn/slot are the authoritative source for that). Leading the
            # filename with it means a plain directory listing sorts chronologically.
            timestamp = datetime.now().strftime("%Y%m%dT%H%M%S_%f")
            base_name = (
                f"srs_{timestamp}_rnti{header['rnti']:#06x}_sfn{header['sfn']}_slot{header['slot_index']}_"
                f"{count:06d}"
            )
            np.save(out_dir / f"{base_name}.npy", iq)
            with open(out_dir / f"{base_name}.json", "w") as f:
                json.dump(header, f, indent=2)

            rnti_counts[header["rnti"]] = rnti_counts.get(header["rnti"], 0) + 1
            count += 1
            last_summary = (
                f"rnti={header['rnti']:#06x} sfn={header['sfn']} slot={header['slot_index']} "
                f"symb=[{header['start_symbol']}, {header['start_symbol'] + header['nof_symbols']}) "
                f"ports={header['nof_ports']} subc={header['nof_subc']} comb={header['comb_size']}/"
                f"{header['comb_offset']} seq_id={header['sequence_id']} -> {base_name}.npy"
            )

            if args.verbose:
                print(f"[{count}] {last_summary}", file=sys.stderr)
            elif live_status:
                # Redraw the whole status block in place: move the cursor back up to the first line of the
                # previous block, then rewrite every line, clearing each with \033[K so a shorter line (e.g. an
                # occasion count dropping back to fewer digits) doesn't leave stale trailing characters. The block
                # only ever grows (RNTIs are never forgotten), so prev_nof_lines is always <= the new line count -
                # moving up by prev_nof_lines never overshoots into unrelated terminal content above.
                lines = format_status_lines(rnti_counts, count, last_summary)
                if prev_nof_lines:
                    sys.stderr.write(f"\033[{prev_nof_lines}A")
                for line in lines:
                    sys.stderr.write(f"\r\033[K{line}\n")
                sys.stderr.flush()
                prev_nof_lines = len(lines)
            else:
                # Not an interactive terminal (e.g. redirected to a file) - in-place redraw doesn't make sense, and
                # reprinting the whole growing RNTI list every occasion would flood the log, so just append the
                # aggregate line; the full per-RNTI breakdown is still shown once at the end, on exit.
                print(format_status_lines(rnti_counts, count, last_summary)[0], file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        # linger=0 so closing doesn't block trying to flush anything on an already-idle PULL socket.
        socket.close(linger=0)
        context.term()

    print(f"\nStopped after saving {count} occasion(s) from {len(rnti_counts)} RNTI(s) to {out_dir}.", file=sys.stderr)
    for line in format_status_lines(rnti_counts, count, "")[1:]:
        print(line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
