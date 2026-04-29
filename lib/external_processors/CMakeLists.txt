# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_library(external_processor_example_impl
        external_ul_processor_example_impl.cpp
        tap_ul_resource_grid_epre_zmq.cpp)
target_include_directories(external_processor_example_impl PRIVATE ${OCUDU_UPPER_PHY_TAP_SRC_DIR}/include)

add_library(external_processors external_processor_factories.cpp)
target_include_directories(external_processors PRIVATE ${OCUDU_UPPER_PHY_TAP_SRC_DIR}/include)
target_link_libraries(external_processors
        external_processor_example_impl
        zmq
        ocudu_support)
