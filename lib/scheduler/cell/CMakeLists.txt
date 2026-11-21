# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_library(sched_cell OBJECT
        vrb_alloc.cpp
        resource_block_group.cpp
        resource_grid.cpp
        scheduler_prb.cpp
        cell_harq_manager.cpp)
# [netsys] The HARQ manager reports the per-TB HARQ statistics to the dumper.
target_link_libraries(sched_cell sched_harq_tb_dump)
