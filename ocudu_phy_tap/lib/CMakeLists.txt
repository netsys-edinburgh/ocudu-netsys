# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


# Add the upper PHY tap library.
add_library(ocudu_upper_phy_tap OBJECT phy_tap_factories.cpp)
target_include_directories(ocudu_upper_phy_tap PRIVATE ${OCUDU_UPPER_PHY_TAP_SRC_DIR}/include)
target_link_libraries(ocudu_upper_phy_tap external_processors)
target_link_libraries(ocudu_upper_phy ocudu_upper_phy_tap)

# Add the pre-processor definition for the upper PHY tap library into the upper PHY.
target_compile_definitions(ocudu_upper_phy PRIVATE -DSRSRAN_HAS_PHY_TAP)

add_subdirectory(external_processors)