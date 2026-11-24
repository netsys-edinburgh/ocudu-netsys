#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Wire and file format of the per-TB HARQ statistics streamed by the gNB (`harq_tb_dump` option).

It must be kept in sync with `lib/scheduler/logging/harq_tb_dumper.h`.

Each ZeroMQ message has two parts: a 28-byte batch header, followed by `nof_records` records of 34 bytes each. All the
fields are little-endian. The consumer writes the records back to back into a .bin file, which can be loaded directly
with numpy using `record_dtype()`.
"""

import struct

#: Magic number of the batch header ("HARQ" in ASCII).
MAGIC = 0x48415251
#: Supported wire format version.
VERSION = 1

#: Batch header layout (struct module syntax) and field names.
HEADER_FORMAT = "<IHHIQQ"
HEADER_FIELDS = ("magic", "version", "record_size", "nof_records", "seq", "nof_dropped_total")
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

#: Record layout (struct module syntax) and field names, in wire order.
RECORD_FORMAT = "<QIIIIHHBBBBBB"
RECORD_FIELDS = (
    "wall_time_ns",
    "first_tx_count",
    "ack_count",
    "tbs_bytes",
    "t_ack_us",
    "rnti",
    "ue_index",
    "h_id",
    "dir",
    "result",
    "nof_retxs",
    "max_retxs",
    "numerology",
)
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

assert HEADER_SIZE == 28, f"Unexpected header size {HEADER_SIZE}"
assert RECORD_SIZE == 34, f"Unexpected record size {RECORD_SIZE}"
assert len(RECORD_FIELDS) == len(RECORD_FORMAT) - 1, "One field name per struct code"

#: Names of the values of the `dir` field.
DIRECTIONS = {0: "DL", 1: "UL"}
#: Names of the values of the `result` field.
RESULTS = {0: "ack", 1: "max_retx", 2: "cancelled", 3: "retx_timeout", 4: "feedback_timeout", 5: "reset"}


class FormatError(ValueError):
    """Raised when a message does not follow this format."""


def parse_header(raw: bytes) -> dict:
    """Decodes and validates a batch header."""
    if len(raw) != HEADER_SIZE:
        raise FormatError(f"header has {len(raw)} bytes instead of {HEADER_SIZE}")
    header = dict(zip(HEADER_FIELDS, struct.unpack(HEADER_FORMAT, raw)))
    if header["magic"] != MAGIC:
        raise FormatError(f"bad magic number {header['magic']:#x}, expected {MAGIC:#x}")
    if header["version"] != VERSION:
        raise FormatError(f"wire format version {header['version']} is not supported, expected {VERSION}")
    if header["record_size"] != RECORD_SIZE:
        raise FormatError(f"record size {header['record_size']} does not match {RECORD_SIZE}")
    return header


def record_dtype():
    """Returns the numpy dtype of one record. Only the reader needs numpy, so it is imported here."""
    import numpy as np

    numpy_types = {"Q": "<u8", "I": "<u4", "H": "<u2", "B": "u1"}
    dtype = np.dtype([(name, numpy_types[code]) for name, code in zip(RECORD_FIELDS, RECORD_FORMAT[1:])])
    assert dtype.itemsize == RECORD_SIZE, f"Unexpected numpy record size {dtype.itemsize}"
    return dtype


def slot_to_sfn_slot(count: int, numerology: int) -> tuple:
    """Splits a slot count, as given by slot_point::count(), into its SFN and its slot index within the frame."""
    slots_per_frame = 10 << numerology
    return count // slots_per_frame, count % slots_per_frame
