# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_library(ocudu_mac_translator_helpers STATIC helpers.cpp)

add_executable(mac_fapi_pdcch_adaptor_test dl_pdcch_pdu_test.cpp)
target_link_libraries(mac_fapi_pdcch_adaptor_test ocudu_mac_fapi_adaptors ocudu_support ocudulog ocudu_mac_translator_helpers gtest gtest_main)
target_include_directories(mac_fapi_pdcch_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_pdcch_adaptor_test)

add_executable(mac_fapi_pdsch_adaptor_test dl_pdsch_pdu_test.cpp)
target_link_libraries(mac_fapi_pdsch_adaptor_test ocudu_mac_fapi_adaptors ocudu_fapi_precoding_matrix_tools ocudu_support ocudulog ocudu_mac_translator_helpers gtest gtest_main)
target_include_directories(mac_fapi_pdsch_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_pdsch_adaptor_test)

add_executable(mac_fapi_ssb_adaptor_test dl_ssb_pdu_test.cpp)
target_link_libraries(mac_fapi_ssb_adaptor_test ocudu_mac_fapi_adaptors ocudu_support ocudulog ocudu_mac_translator_helpers gtest gtest_main)
target_include_directories(mac_fapi_ssb_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_ssb_adaptor_test)

add_executable(mac_fapi_prs_adaptor_test dl_prs_pdu_test.cpp)
target_link_libraries(mac_fapi_prs_adaptor_test ocudu_mac_fapi_adaptors ocudu_fapi_precoding_matrix_tools ocudu_support ocudulog ocudu_mac_translator_helpers gtest gtest_main)
target_include_directories(mac_fapi_prs_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_prs_adaptor_test)

add_executable(mac_fapi_prach_adaptor_test ul_prach_pdu_test.cpp)
target_link_libraries(mac_fapi_prach_adaptor_test ocudu_mac_fapi_adaptors ocudu_support gtest gtest_main ocudu_mac_translator_helpers)
target_include_directories(mac_fapi_prach_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_prach_adaptor_test)

add_executable(mac_fapi_pucch_adaptor_test ul_pucch_pdu_test.cpp)
target_link_libraries(mac_fapi_pucch_adaptor_test ocudu_mac_fapi_adaptors ocudu_support gtest gtest_main ocudu_mac_translator_helpers)
target_include_directories(mac_fapi_pucch_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
# The PUCCH repetition test is registered separately below, so that it can carry the MVP-FUNC-BW-17-1 label.
# --gtest_fail_if_no_test_selected makes a filter that no longer matches anything (e.g. after a test rename) fail
# instead of silently reporting a passing, empty run.
gtest_discover_tests(mac_fapi_pucch_adaptor_test
        TEST_FILTER "-mac_fapi_ul_pucch_format1_pdu_conversor_test.multi_slot_tx_indicator_is_forwarded")

add_test(NAME mac_fapi_ul_pucch_format1_pdu_conversor_test.multi_slot_tx_indicator_is_forwarded
         COMMAND mac_fapi_pucch_adaptor_test
                 "--gtest_filter=mac_fapi_ul_pucch_format1_pdu_conversor_test.multi_slot_tx_indicator_is_forwarded"
                 --gtest_fail_if_no_test_selected)
set_tests_properties(
    mac_fapi_ul_pucch_format1_pdu_conversor_test.multi_slot_tx_indicator_is_forwarded
    PROPERTIES LABELS "MVP-FUNC-BW-17-1")

add_executable(mac_fapi_pusch_adaptor_test ul_pusch_pdu_test.cpp)
target_link_libraries(mac_fapi_pusch_adaptor_test ocudu_mac_fapi_adaptors ocudu_support ocudu_fapi_uci_part2_tools gtest gtest_main ocudu_mac_translator_helpers)
target_include_directories(mac_fapi_pusch_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_pusch_adaptor_test)

add_executable(mac_fapi_srs_adaptor_test ul_srs_pdu_test.cpp)
target_link_libraries(mac_fapi_srs_adaptor_test ocudu_mac_fapi_adaptors ocudu_support gtest gtest_main ocudu_mac_translator_helpers)
target_include_directories(mac_fapi_srs_adaptor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi_adaptor/mac/p7/pdu_translators)
gtest_discover_tests(mac_fapi_srs_adaptor_test)
