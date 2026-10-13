# SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

add_executable(ru_sdr_executor_mapper_test ru_sdr_executor_mapper_test.cpp)
target_link_libraries(ru_sdr_executor_mapper_test ocudu_ru_sdr ocudu_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(ru_sdr_executor_mapper_test)
