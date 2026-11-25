#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Batch header of the gNB statistics streams (`harq_tb_dump` and `rlc_sdu_dump` options), and shared helpers.

The header must match `stats_batch_header` in `include/ocudu/support/netsys_stats/stats_zmq_sender.h`. Each ZeroMQ
message has two parts: a 28-byte batch header, followed by `nof_records` records of `record_size` bytes each. All the
fields are little-endian. The magic number of the header identifies the stream, whose record layout is defined in its
own module: harq_tb_format.py or rlc_sdu_format.py. Each of these modules defines NAME, MAGIC, VERSION, RECORD_FORMAT,
RECORD_FIELDS and RECORD_SIZE.

The consumer writes the records of a stream back to back into `<NAME>_<start time>.bin`, next to a .json file that
summarizes the run. The helpers at the end of this module are used by the readers, and need numpy.
"""

import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

import harq_tb_format
import rlc_sdu_format

#: Batch header layout (struct module syntax) and field names.
HEADER_FORMAT = "<IHHIQQ"
HEADER_FIELDS = ("magic", "version", "record_size", "nof_records", "seq", "nof_dropped_total")
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

assert HEADER_SIZE == 28, f"Unexpected header size {HEADER_SIZE}"

#: Known streams, by magic number.
STREAMS = {stream.MAGIC: stream for stream in (harq_tb_format, rlc_sdu_format)}


class FormatError(ValueError):
    """Raised when a message does not follow this format."""


def parse_header(raw: bytes):
    """Decodes and validates a batch header. Returns the header, as a dict, and the module of its stream."""
    if len(raw) != HEADER_SIZE:
        raise FormatError(f"header has {len(raw)} bytes instead of {HEADER_SIZE}")
    header = dict(zip(HEADER_FIELDS, struct.unpack(HEADER_FORMAT, raw)))
    stream = STREAMS.get(header["magic"])
    if stream is None:
        raise FormatError(f"unknown magic number {header['magic']:#x}")
    if header["version"] != stream.VERSION:
        raise FormatError(
            f"{stream.NAME} wire format version {header['version']} is not supported, expected {stream.VERSION}"
        )
    if header["record_size"] != stream.RECORD_SIZE:
        raise FormatError(f"{stream.NAME} record size {header['record_size']} does not match {stream.RECORD_SIZE}")
    return header, stream


def record_dtype(stream):
    """Returns the numpy dtype of one record of the given stream module."""
    import numpy as np

    numpy_types = {"Q": "<u8", "I": "<u4", "H": "<u2", "B": "u1"}
    codes = stream.RECORD_FORMAT[1:]
    dtype = np.dtype([(name, numpy_types[code]) for name, code in zip(stream.RECORD_FIELDS, codes)])
    assert dtype.itemsize == stream.RECORD_SIZE, f"Unexpected numpy record size {dtype.itemsize}"
    return dtype


def find_bin_files(paths, stream):
    """Expands directories into the .bin files of the given stream that they contain."""
    files = []
    for path in paths:
        if path.is_dir():
            files.extend(sorted(path.glob(f"{stream.NAME}_*.bin")))
        else:
            files.append(path)
    return files


def load_records(bin_path: Path, stream):
    """Loads the records of one .bin file. A truncated last record, e.g. from a killed consumer, is ignored."""
    import numpy as np

    size = bin_path.stat().st_size
    if size % stream.RECORD_SIZE:
        print(f"WARNING: {bin_path} ends with {size % stream.RECORD_SIZE} bytes of a truncated record", file=sys.stderr)
    return np.fromfile(bin_path, dtype=record_dtype(stream), count=size // stream.RECORD_SIZE)


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


def print_time_stats(label: str, time_us) -> None:
    """Prints the mean, percentiles and maximum of a set of durations given in microseconds."""
    import numpy as np

    time_ms = np.asarray(time_us) / 1000.0
    p50, p90, p99 = np.percentile(time_ms, [50, 90, 99])
    print(
        f"    {label}: mean={time_ms.mean():.2f} ms  p50={p50:.2f} ms  p90={p90:.2f} ms  "
        f"p99={p99:.2f} ms  max={time_ms.max():.2f} ms"
    )
