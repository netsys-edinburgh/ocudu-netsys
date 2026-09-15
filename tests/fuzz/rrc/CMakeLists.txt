# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

set_directory_properties(PROPERTIES LABELS "fuzz|rrc")

# ---------------------------------------------------------------------------
# RRC UE uplink fuzzer
#
# Drives an rrc_ue_impl directly, without a CU-CP, PDCP or F1AP around it, so that the fuzzer
# controls the integrity_verified flag and reaches the TS 38.331 Annex B1 gating branches.
# ---------------------------------------------------------------------------
add_executable(rrc_ue_fuzzer rrc_ue_fuzzer.cpp)
target_include_directories(rrc_ue_fuzzer PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(rrc_ue_fuzzer
    ocudu_rrc
    ocudu_cu_cp
    ocudu_support
    ocudulog
    ${FUZZ_SANITIZER_FLAGS_LINK}
)
target_compile_options(rrc_ue_fuzzer PRIVATE ${FUZZ_SANITIZER_FLAGS_COMPILE})

add_dependencies(fuzz_targets rrc_ue_fuzzer)

