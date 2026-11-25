#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Record format of the per-SDU RLC AM reTx statistics streamed by the gNB (`rlc_sdu_dump` option).

It must be kept in sync with `rlc_sdu_record` in `lib/rlc/rlc_sdu_dumper.h`. See stats_format.py for the batch header.
"""

import struct

#: Name of the stream, used as the prefix of its files.
NAME = "rlc_sdu"
#: Magic number of the batch header ("RLCS" in ASCII).
MAGIC = 0x524C4353
#: Supported wire format version.
VERSION = 1

#: Record layout (struct module syntax) and field names, in wire order.
RECORD_FORMAT = "<QIIIHBBBBBB"
RECORD_FIELDS = (
    "wall_time_ns",
    "t_us",
    "sn",
    "sdu_len",
    "ue_index",
    "rb_type",
    "rb_id",
    "dir",
    "result",
    "nof_retxs",
    "max_retx_thresh",
)
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

assert RECORD_SIZE == 28, f"Unexpected record size {RECORD_SIZE}"
assert len(RECORD_FIELDS) == len(RECORD_FORMAT) - 1, "One field name per struct code"

#: Value of `nof_retxs` and `max_retx_thresh` when they are unknown, i.e., for UL SDUs.
UNKNOWN = 0xFF

#: Names of the values of the `dir` field.
DIRECTIONS = {0: "DL", 1: "UL"}
#: Names of the values of the `result` field.
RESULTS = {0: "ack", 1: "max_retx", 2: "recovered"}
#: Names of the values of the `rb_type` field.
RB_TYPES = {0: "srb", 1: "drb"}


def rb_name(rb_type: int, rb_id: int) -> str:
    """Formats a radio bearer as e.g. drb1."""
    return f"{RB_TYPES.get(rb_type, '?')}{rb_id}"
