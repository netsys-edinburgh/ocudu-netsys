#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Record format of the per-TB HARQ statistics streamed by the gNB (`harq_tb_dump` option).

It must be kept in sync with `harq_tb_record` in `lib/scheduler/logging/harq_tb_dumper.h`. See stats_format.py for the
batch header.
"""

import struct

#: Name of the stream, used as the prefix of its files.
NAME = "harq_tb"
#: Magic number of the batch header ("HARQ" in ASCII).
MAGIC = 0x48415251
#: Supported wire format version.
VERSION = 1

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

assert RECORD_SIZE == 34, f"Unexpected record size {RECORD_SIZE}"
assert len(RECORD_FIELDS) == len(RECORD_FORMAT) - 1, "One field name per struct code"

#: Names of the values of the `dir` field.
DIRECTIONS = {0: "DL", 1: "UL"}
#: Names of the values of the `result` field.
RESULTS = {0: "ack", 1: "max_retx", 2: "cancelled", 3: "retx_timeout", 4: "feedback_timeout", 5: "reset"}


def slot_to_sfn_slot(count: int, numerology: int) -> tuple:
    """Splits a slot count, as given by slot_point::count(), into its SFN and its slot index within the frame."""
    slots_per_frame = 10 << numerology
    return count // slots_per_frame, count % slots_per_frame
