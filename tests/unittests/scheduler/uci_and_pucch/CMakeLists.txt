# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.



add_executable(uci_pucch_sched_test
        uci_indication_selector_test.cpp
        pucch_alloc_common_harq_test.cpp
        pucch_alloc_ded_resources_test.cpp
        pucch_alloc_format_0_test.cpp
        pucch_alloc_harq_sr_csi_test.cpp
        pucch_alloc_repetition_test.cpp
        pucch_collision_manager_test.cpp
        scheduler_uci_indication_test.cpp
        uci_allocator_test.cpp
        uci_scheduling_test.cpp
        uci_test_utils.cpp)
target_link_libraries(uci_pucch_sched_test
        ocudu_sched
        scheduler_test_suite
        ocudulog
        sched_config
        mac_configuration_helpers
        sched_test_doubles
        sched_rrm
        gtest
        ocudu_gtest_main)
# The PUCCH repetition tests are registered separately below, so that they can carry the MVP-FUNC-BW-17-1 label.
# --gtest_fail_if_no_test_selected makes a filter that no longer matches anything (e.g. after a test rename) fail
# instead of silently reporting a passing, empty run.
gtest_discover_tests(uci_pucch_sched_test
        TEST_FILTER "-pucch_alloc_repetition*:uci_indication_selector_test.*burst*")

add_test(NAME pucch_alloc_repetition_test
         COMMAND uci_pucch_sched_test "--gtest_filter=pucch_alloc_repetition*" --gtest_fail_if_no_test_selected)
add_test(NAME uci_indication_selector_repetition_test
         COMMAND uci_pucch_sched_test "--gtest_filter=uci_indication_selector_test.*burst*"
                 --gtest_fail_if_no_test_selected)
set_tests_properties(
    pucch_alloc_repetition_test
    uci_indication_selector_repetition_test
    PROPERTIES LABELS "MVP-FUNC-BW-17-1")
