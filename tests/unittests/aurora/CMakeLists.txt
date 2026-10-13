set_directory_properties(PROPERTIES LABELS "aurora")

add_executable(aurora_core_test aurora_core_test.cpp)
target_include_directories(aurora_core_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/lib/aurora)
target_link_libraries(aurora_core_test ocudu_aurora GTest::gtest ocudu_gtest_main)
add_test(aurora_core_test aurora_core_test)

add_executable(aurora_e2e_test aurora_e2e_test.cpp)
target_include_directories(aurora_e2e_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/lib/aurora)
target_link_libraries(aurora_e2e_test ocudu_aurora GTest::gtest ocudu_gtest_main)
add_test(aurora_e2e_test aurora_e2e_test)

add_executable(aurora_thread_ebpf_merge_test aurora_thread_ebpf_merge_test.cpp)
target_link_libraries(aurora_thread_ebpf_merge_test ocudu_aurora_thread GTest::gtest ocudu_gtest_main)
add_test(aurora_thread_ebpf_merge_test aurora_thread_ebpf_merge_test)
