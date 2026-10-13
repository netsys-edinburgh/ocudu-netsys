# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

add_executable(pusch_processor_validator_test pusch_processor_validator_test.cpp)
target_link_libraries(pusch_processor_validator_test
        ocudu_channel_equalizer
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        ocudu_transform_precoding
        GTest::gtest
        GTest::gtest_main)
add_test(pusch_processor_validator_test pusch_processor_validator_test)

add_executable(pusch_processor_unittest pusch_processor_unittest.cpp)
target_include_directories(pusch_processor_unittest PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(pusch_processor_unittest ocudu_channel_processors ocudulog GTest::gtest GTest::gtest_main)
add_test(pusch_processor_unittest pusch_processor_unittest)

add_executable(pusch_processor_sinr_source_test pusch_processor_sinr_source_test.cpp)
target_include_directories(pusch_processor_sinr_source_test PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/unittests
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_processor_sinr_source_test ocudu_channel_processors ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_processor_sinr_source_test)

add_executable(pusch_demodulator_gpu_stats_test pusch_demodulator_gpu_stats_test.cpp)
target_include_directories(pusch_demodulator_gpu_stats_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_demodulator_gpu_stats_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_demodulator_gpu_stats_test)

add_executable(pusch_acceleration_runtime_options_test pusch_acceleration_runtime_options_test.cpp)
target_include_directories(pusch_acceleration_runtime_options_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_acceleration_runtime_options_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_acceleration_runtime_options_test)

add_executable(pusch_sinr_comparison_test pusch_sinr_comparison_test.cpp)
target_link_libraries(pusch_sinr_comparison_test
        ocudu_channel_equalizer
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        ocudu_transform_precoding
        gtest
        gtest_main)
add_test(pusch_sinr_comparison_test pusch_sinr_comparison_test)
# The test drives the GPU on CUDA builds. Serialise it with the other GPU tests, see
# tests/unittests/phy/upper/channel_coding/ldpc/CMakeLists.txt.
if (CUDA_ACCEL_FOUND)
    # The GPU wait timeout case holds the demodulator stream behind a gate written through the CUDA driver API.
    target_link_libraries(pusch_sinr_comparison_test CUDA::cudart CUDA::cuda_driver)
    target_compile_definitions(pusch_sinr_comparison_test PRIVATE OCUDU_TEST_CUDA_STREAM_GATE)
    set_tests_properties(pusch_sinr_comparison_test PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif ()

# The batched GPU decoding of the PUSCH decoder only exists in CUDA builds. The test replaces the GPU decoder with a
# spy, so it does not use the GPU.
if (CUDA_ACCEL_FOUND)
    add_executable(pusch_decoder_batch_sequencing_test pusch_decoder_batch_sequencing_test.cpp)
    target_include_directories(pusch_decoder_batch_sequencing_test PRIVATE
            ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
    target_compile_definitions(pusch_decoder_batch_sequencing_test PRIVATE ENABLE_CUDA)
    target_link_libraries(pusch_decoder_batch_sequencing_test
            ocudu_pusch_processor
            ocudu_channel_coding
            ocudulog
            GTest::gtest
            GTest::gtest_main)
    gtest_discover_tests(pusch_decoder_batch_sequencing_test)
endif ()
