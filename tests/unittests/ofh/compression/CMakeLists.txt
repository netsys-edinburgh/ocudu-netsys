# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set_directory_properties(PROPERTIES LABELS "compression")

add_executable(ofh_beamforming_weights_compressor_test ofh_beamforming_weights_compressor_test.cpp)
target_include_directories(ofh_beamforming_weights_compressor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/ofh/compression ${CMAKE_SOURCE_DIR})
target_link_libraries(ofh_beamforming_weights_compressor_test ocudu_ofh_compression ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(ofh_beamforming_weights_compressor_test)

add_executable(compression_validator_test compression_validator_test.cpp)
target_link_libraries(compression_validator_test ocudu_ofh_compression ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(compression_validator_test)

add_executable(ofh_iq_compression_bfp_test ofh_iq_compression_bfp_test.cpp)
target_link_libraries(ofh_iq_compression_bfp_test ocudu_ofh_compression ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(ofh_iq_compression_bfp_test)

if (CUDA_ACCEL_FOUND)
    add_executable(ofh_iq_compression_cuda_test ofh_iq_compression_cuda_test.cpp)
    # The CPU fallback test inspects the accelerated compressor implementation directly.
    target_include_directories(ofh_iq_compression_cuda_test PRIVATE
            ${CMAKE_SOURCE_DIR}/lib/ofh/compression
            ${CMAKE_SOURCE_DIR}/lib/phy/upper)
    target_link_libraries(ofh_iq_compression_cuda_test
            ocudu_ofh_compression
            ocudu_upper_phy
            ocudu_phy_support
            ocudu_support
            CUDA::cuda_driver
            GTest::gtest
            GTest::gtest_main)
    target_compile_definitions(ofh_iq_compression_cuda_test PRIVATE ENABLE_CUDA)
    # Serialise GPU tests: the CUDA runtime rejects allocations with cudaErrorDevicesUnavailable while several
    # exclusive device contexts are active, which surfaces as intermittent failures rather than real defects.
    gtest_discover_tests(ofh_iq_compression_cuda_test PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif()
