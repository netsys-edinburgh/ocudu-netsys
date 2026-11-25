# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# [netsys] Streaming of statistics records over ZeroMQ, shared by the HARQ and RLC dumps.
add_library(netsys_stats STATIC stats_zmq_sender.cpp)
target_link_libraries(netsys_stats ocudu_support fmt zmq)
