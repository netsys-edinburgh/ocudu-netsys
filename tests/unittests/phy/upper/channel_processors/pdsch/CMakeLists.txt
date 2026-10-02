# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

add_executable(pdsch_processor_unittest pdsch_processor_unittest.cpp)
target_link_libraries(pdsch_processor_unittest ocudu_channel_processors ocudulog GTest::gtest GTest::gtest_main)
add_test(pdsch_processor_unittest pdsch_processor_unittest)

add_executable(pdsch_processor_validator_test pdsch_processor_validator_test.cpp)
target_link_libraries(pdsch_processor_validator_test
        ocudu_channel_equalizer
        ocudu_channel_precoder
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        GTest::gtest
        GTest::gtest_main)
add_test(pdsch_processor_validator_test pdsch_processor_validator_test)

if (CUDA_ACCEL_FOUND)
  # CUDA PDSCH symbol-generation kernels versus the host rate matcher, scrambler and modulation mapper for every
  # modulation order, including 1024QAM.
  add_executable(pdsch_cuda_modulation_order_test pdsch_cuda_modulation_order_test.cpp)
  target_link_libraries(pdsch_cuda_modulation_order_test
          ocudu_ldpc_cuda
          ocudu_channel_coding
          ocudu_channel_modulation
          ocudu_sequence_generators
          ocudulog
          GTest::gtest
          GTest::gtest_main)
  add_test(pdsch_cuda_modulation_order_test pdsch_cuda_modulation_order_test)
  set_tests_properties(pdsch_cuda_modulation_order_test PROPERTIES LABELS "cuda;pdsch" RESOURCE_LOCK ocudu_cuda_device)
endif ()
