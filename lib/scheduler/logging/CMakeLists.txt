# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

set(SOURCES scheduler_result_logger.cpp cell_metrics_handler.cpp scheduler_event_logger.cpp cell_event_tracer.cpp)
add_library(scheduler_logger STATIC ${SOURCES})
target_link_libraries(scheduler_logger ocudu_sched ocudu_ran ocudu_support ocudulog)

# [netsys] Per-TB HARQ statistics dump over ZeroMQ.
add_library(sched_harq_tb_dump STATIC harq_tb_dumper.cpp)
target_link_libraries(sched_harq_tb_dump ocudu_support fmt zmq)
