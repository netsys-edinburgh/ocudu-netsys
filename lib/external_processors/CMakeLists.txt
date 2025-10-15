#
# Copyright 2021-2025 Software Radio Systems Limited
#
# By using this file, you agree to the terms and conditions set
# forth in the LICENSE file which can be found at the top level of
# the distribution.
#

add_library(external_processor_example_impl
        external_ul_processor_example_impl.cpp
        tap_ul_resource_grid_epre_zmq.cpp)
target_include_directories(external_processor_example_impl PRIVATE ${SRSRAN_UPPER_PHY_TAP_SRC_DIR}/include)

add_library(external_processors external_processor_factories.cpp)
target_include_directories(external_processors PRIVATE ${SRSRAN_UPPER_PHY_TAP_SRC_DIR}/include)
target_link_libraries(external_processors
        external_processor_example_impl
        zmq
        srsran_support)
