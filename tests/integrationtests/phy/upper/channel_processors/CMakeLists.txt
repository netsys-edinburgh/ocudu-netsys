# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy;tsan;NO_MEMCHECK")

add_executable(pxsch_bler_test pxsch_bler_test.cpp)

set(PXSCH_BLER_TEST_LIBRARIES ocudu_ran
        ocudu_upper_phy
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec)

if (ENABLE_PDSCH_HWACC AND ENABLE_PUSCH_HWACC)
    set_source_files_properties(pxsch_bler_test.cpp pxsch_bler_test_factories.cpp PROPERTIES COMPILE_DEFINITIONS "HWACC_PDSCH_ENABLED; HWACC_PUSCH_ENABLED")
    list(APPEND PXSCH_BLER_TEST_LIBRARIES ocudu_hal_pusch
            ocudu_hal_pdsch
            ocudu_hal_bbdev)
endif (ENABLE_PDSCH_HWACC AND ENABLE_PUSCH_HWACC)

add_library(ocudu_pxsch_bler_test_helpers STATIC
        pxsch_bler_test_channel_emulator.cpp
        pxsch_bler_test_factories.cpp
        pxsch_bler_test_parse.cpp)
target_link_libraries(ocudu_pxsch_bler_test_helpers ${PXSCH_BLER_TEST_LIBRARIES})

target_link_libraries(pxsch_bler_test ocudu_pxsch_bler_test_helpers)
add_test(pxsch_bler_test pxsch_bler_test -R 10)
# BLER test - single codeblock (1 RB), 32 repetitions.
add_test(pxsch_bler_test_1CBs_32reps pxsch_bler_test -m 0 -R 10 -V [0,2,3,1,0,2,3,1,0,2,3,1,0,2,3,1,0,2,3,1,0,2,3,1,0,2,3,1,0,2,3,1] -B 1)
# BLER test - many codeblocks, one thread.
add_test(pxsch_bler_test_manyCBs_4rep pxsch_bler_test -m 28 -R 10 -V [0,2,3,1] -j 1)

add_executable(pxsch_chain_test pxsch_chain_test.cpp)
target_link_libraries(pxsch_chain_test
        ocudu_ran
        ocudu_channel_processors
        ocudulog
        ocuduvec
        gtest
        gtest_main
)
gtest_discover_tests(pxsch_chain_test)

add_executable(pusch_gpu_softbit_scale_test pusch_gpu_softbit_scale_test.cpp)
target_link_libraries(pusch_gpu_softbit_scale_test ocudu_pxsch_bler_test_helpers gtest gtest_main)
add_test(pusch_gpu_softbit_scale_test pusch_gpu_softbit_scale_test)

add_executable(pusch_gpu_cpu_comparison_test pusch_gpu_cpu_comparison_test.cpp)
target_link_libraries(pusch_gpu_cpu_comparison_test ocudu_pxsch_bler_test_helpers)
add_test(pusch_gpu_cpu_comparison_test pusch_gpu_cpu_comparison_test -R 10)
add_test(pusch_gpu_cpu_cfo_interpolation_test pusch_gpu_cpu_comparison_test -L -P 27 -R 10 -S 30 -N 1)
add_test(pusch_gpu_cpu_sync_sinr_test pusch_gpu_cpu_comparison_test -L -H -P 27 -R 10 -S 30 -N 1)
# Resident GPU DM-RS SINR against the host channel estimator, with and without the DM-RS power boost.
add_test(pusch_gpu_cpu_estimator_sinr_test pusch_gpu_cpu_comparison_test -C -F TDLA -P 52 -R 10 -S 10,25 -N 1,2,4)
add_test(pusch_gpu_cpu_estimator_sinr_no_boost_test
         pusch_gpu_cpu_comparison_test -C -D 1 -P 52 -R 10 -S 10,25 -N 1,2,4)
# Nothing received: the GPU must not report a SINR it did not measure, nor a decoded transport block.
add_test(pusch_gpu_empty_grid_sinr_test pusch_gpu_cpu_comparison_test -Z -P 52 -R 5 -S 10 -N 1,2)
set_tests_properties(pusch_gpu_cpu_estimator_sinr_test pusch_gpu_cpu_estimator_sinr_no_boost_test
                     pusch_gpu_empty_grid_sinr_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(pdsch_gpu_e2e_test pdsch_gpu_e2e_test.cpp)
target_link_libraries(pdsch_gpu_e2e_test
        ocudu_upper_phy
        ocudu_ran
        ocudu_channel_processors
        ocudu_phy_support
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec
        gtest
        gtest_main
)
add_test(pdsch_gpu_e2e_test pdsch_gpu_e2e_test)
if(CUDA_ACCEL_FOUND)
  target_include_directories(pdsch_gpu_e2e_test PRIVATE
          ${CMAKE_SOURCE_DIR}/lib/phy/upper
          ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc)
  target_link_libraries(pdsch_gpu_e2e_test ocudu_ldpc_cuda ocudu_ofh_compression CUDA::cudart)
  target_compile_definitions(pdsch_gpu_e2e_test PRIVATE ENABLE_CUDA)
  # Device-grid PDSCH mapping, identity and codebook precoding. Discrete GPUs enable the device mapper only on request
  # (OCUDU_PDSCH_DIRECT_DEVICE_GRID, or a block processor created for device grid mapping), so these run in a process
  # of their own with the variable set.
  add_test(NAME pdsch_gpu_device_grid_precoding_test
           COMMAND pdsch_gpu_e2e_test --gtest_filter=Layers/PdschGpuDeviceGridWriter.*:PdschGpuDeviceGridWriterPmi*)
  set_tests_properties(pdsch_gpu_device_grid_precoding_test PROPERTIES ENVIRONMENT OCUDU_PDSCH_DIRECT_DEVICE_GRID=1)
  # Codebook precoding through captured CUDA graphs: a transmission with a new PMI must not replay another's graph.
  add_test(NAME pdsch_gpu_pmi_cuda_graph_test
           COMMAND pdsch_gpu_e2e_test --gtest_filter=PdschGpuDeviceGridWriterPmi.ReusedProcessorAppliesEachTransmissionPrecoding)
  set_tests_properties(pdsch_gpu_pmi_cuda_graph_test
                       PROPERTIES ENVIRONMENT "OCUDU_PDSCH_CUDA_GRAPHS=1;OCUDU_PDSCH_DIRECT_DEVICE_GRID=1")
  add_test(NAME pdsch_gpu_issue_1_regression
           COMMAND pdsch_gpu_e2e_test --gtest_filter=PdschGpuIssueProfile.*)
  set_tests_properties(pdsch_gpu_issue_1_regression PROPERTIES LABELS "cuda;pdsch;regression" TIMEOUT 120)
endif()

add_executable(pdsch_gpu_latency_benchmark pdsch_gpu_latency_benchmark.cpp)
target_link_libraries(pdsch_gpu_latency_benchmark
        ocudu_ran
        ocudu_channel_processors
        ocudu_phy_support
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec
)
if(CUDA_ACCEL_FOUND)
  target_include_directories(pdsch_gpu_latency_benchmark PRIVATE
          ${CMAKE_SOURCE_DIR}/lib/phy/upper
          ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc)
  target_link_libraries(pdsch_gpu_latency_benchmark ocudu_ldpc_cuda ocudu_cuda_grid_snapshot CUDA::cudart)
  target_compile_definitions(pdsch_gpu_latency_benchmark PRIVATE ENABLE_CUDA)
endif()

add_executable(pusch_gpu_cpu_result_parity_test pusch_gpu_cpu_result_parity_test.cpp)
target_link_libraries(pusch_gpu_cpu_result_parity_test ocudu_pxsch_bler_test_helpers)
add_test(pusch_gpu_cpu_result_parity_test pusch_gpu_cpu_result_parity_test)

add_executable(pusch_64qam_stress_test pusch_64qam_stress_test.cpp)
target_link_libraries(pusch_64qam_stress_test ocudu_pxsch_bler_test_helpers)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_64qam_stress_test PRIVATE ENABLE_CUDA)
  # Add include paths to access internal headers for timing profiling
  target_include_directories(pusch_64qam_stress_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
  )
endif()
add_test(pusch_64qam_stress_test pusch_64qam_stress_test)


add_executable(pusch_e2e_pipeline_test pusch_e2e_pipeline_test.cpp)
target_link_libraries(pusch_e2e_pipeline_test ocudu_pxsch_bler_test_helpers)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_pipeline_test PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_e2e_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
  )
  target_link_libraries(pusch_e2e_pipeline_test ocudu_ldpc_cuda CUDA::cudart)
endif()
add_test(pusch_e2e_pipeline_test pusch_e2e_pipeline_test)

if(CUDA_ACCEL_FOUND)
  add_executable(pusch_resident_dematch_scramble_test pusch_resident_dematch_scramble_test.cpp)
  target_include_directories(pusch_resident_dematch_scramble_test PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
  target_link_libraries(pusch_resident_dematch_scramble_test ocudu_ldpc_cuda CUDA::cudart)
  target_compile_definitions(pusch_resident_dematch_scramble_test PRIVATE ENABLE_CUDA)
  add_test(pusch_resident_dematch_scramble_test pusch_resident_dematch_scramble_test)
endif()

add_executable(pusch_mimo_eq_parity_test pusch_mimo_eq_parity_test.cpp)
target_link_libraries(pusch_mimo_eq_parity_test
  ocudu_pxsch_bler_test_helpers
  ocudu_channel_processors
  ocudu_phy_support
  ocudu_upper_phy_support
  ocudu_channel_precoder
  ocudu_channel_equalizer
  ocudu_dft
  ocudulog
  ocuduvec
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_mimo_eq_parity_test PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_mimo_eq_parity_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
  target_link_libraries(pusch_mimo_eq_parity_test ocudu_ldpc_cuda CUDA::cudart)
endif()

add_executable(pusch_e2e_correctness_sweep pusch_e2e_correctness_sweep.cpp)
target_link_libraries(pusch_e2e_correctness_sweep ocudu_pxsch_bler_test_helpers)
target_include_directories(pusch_e2e_correctness_sweep PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_correctness_sweep PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_e2e_correctness_sweep PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
endif()
add_test(pusch_e2e_correctness_sweep pusch_e2e_correctness_sweep)

add_executable(pusch_e2e_sensitivity_sweep pusch_e2e_sensitivity_sweep.cpp)
target_link_libraries(pusch_e2e_sensitivity_sweep ocudu_pxsch_bler_test_helpers)
target_include_directories(pusch_e2e_sensitivity_sweep PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_sensitivity_sweep PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_e2e_sensitivity_sweep PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
endif()
# The built-in default sweep covers 8 PRB/MCS configs up to 273PRB and takes hours: run a reduced set in CI and
# leave the full sweep for manual invocation. See --help for the knobs.
add_test(NAME pusch_e2e_sensitivity_sweep
         COMMAND pusch_e2e_sensitivity_sweep --nof_prb 25 --mcs_index 11,27 --nof_frames 20)
set_tests_properties(pusch_e2e_sensitivity_sweep PROPERTIES TIMEOUT 600)

# GPU tests must not run concurrently with each other. The CUDA runtime rejects allocations with
# cudaErrorDevicesUnavailable while several exclusive device contexts are active, which surfaces as intermittent ctest
# failures that look like defects in the accelerated code. The shared lock lets ctest keep running the rest of the suite
# in parallel while serialising the tests that drive the GPU.
if (CUDA_ACCEL_FOUND)
    set_tests_properties(
            pusch_gpu_cpu_comparison_test
            pusch_gpu_softbit_scale_test
            pusch_gpu_cpu_cfo_interpolation_test
            pusch_gpu_cpu_sync_sinr_test
            pusch_gpu_cpu_estimator_sinr_test
            pusch_gpu_cpu_estimator_sinr_no_boost_test
            pusch_gpu_empty_grid_sinr_test
            pdsch_gpu_e2e_test
            pdsch_gpu_device_grid_precoding_test
            pdsch_gpu_pmi_cuda_graph_test
            pdsch_gpu_issue_1_regression
            pusch_gpu_cpu_result_parity_test
            pusch_64qam_stress_test
            pusch_e2e_pipeline_test
            pusch_resident_dematch_scramble_test
            pusch_e2e_correctness_sweep
            pusch_e2e_sensitivity_sweep
            PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif ()
