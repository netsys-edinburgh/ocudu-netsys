# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_executable(ue_scheduler_test
        fallback_scheduler_test.cpp
        triggered_ul_grant_scheduler_test.cpp
        ue_configuration_test.cpp
        ue_grid_allocator_test.cpp
        ue_link_adaptation_controller_test.cpp
        ue_harq_link_adaptation_test.cpp)
target_link_libraries(ue_scheduler_test
        ocudu_sched
        ocudulog
        mac_configuration_helpers
        scheduler_test_suite
        sched_config
        gtest
        ocudu_gtest_main)
# The PUCCH repetition tests are registered separately below, so that they can carry the MVP-FUNC-BW-17-1 label.
# --gtest_fail_if_no_test_selected makes a filter that no longer matches anything (e.g. after a test rename) fail
# instead of silently reporting a passing, empty run.
gtest_discover_tests(ue_scheduler_test
        TEST_FILTER "-ue_link_adaptation_controller_pucch_rep_test.*")

add_test(NAME ue_link_adaptation_controller_pucch_rep_test
         COMMAND ue_scheduler_test "--gtest_filter=ue_link_adaptation_controller_pucch_rep_test.*"
                 --gtest_fail_if_no_test_selected)
set_tests_properties(ue_link_adaptation_controller_pucch_rep_test PROPERTIES LABELS "MVP-FUNC-BW-17-1")
