#
# Copyright 2021-2025 Software Radio Systems Limited
#
# By using this file, you agree to the terms and conditions set
# forth in the LICENSE file which can be found at the top level of
# the distribution.
#

# Add the upper PHY tap library.
add_library(srsran_upper_phy_tap OBJECT rx_symbol_handler_tap_factory.cpp)
target_include_directories(srsran_upper_phy_tap PRIVATE ${SRSRAN_UPPER_PHY_TAP_SRC_DIR}/include)
target_link_libraries(srsran_upper_phy_tap external_processors)
target_link_libraries(srsran_upper_phy srsran_upper_phy_tap)

# Add the pre-processor definition for the upper PHY tap library into the upper PHY.
target_compile_definitions(srsran_upper_phy PRIVATE -DSRSRAN_HAS_PHY_TAP)

add_subdirectory(external_processors)