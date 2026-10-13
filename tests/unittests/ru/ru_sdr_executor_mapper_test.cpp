// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ru/sdr/ru_sdr_executor_mapper.h"
#include "ocudu/support/executors/manual_task_worker.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

class RuSdrExecutorMapperFixture : public ::testing::Test
{
protected:
  static constexpr unsigned queue_size = 16;

  manual_task_worker radio_exec{queue_size};
  manual_task_worker high_prio_exec{queue_size};
  manual_task_worker tx_exec{queue_size};
  manual_task_worker rx_exec{queue_size};
  manual_task_worker ul_exec{queue_size};
  manual_task_worker prach_exec{queue_size};
};

} // namespace

// The PRACH runs on its dedicated executor when one is configured, and on the downlink executor otherwise.
TEST_F(RuSdrExecutorMapperFixture, dual_profile_uses_the_dedicated_prach_executor)
{
  for (bool dedicated : {false, true}) {
    ru_sdr_executor_mapper_dual_configuration config = {};
    config.radio_exec                                = &radio_exec;
    config.high_prio_executor                        = &high_prio_exec;
    config.baseband_exec.push_back(
        {.tx_exec = &tx_exec, .rx_exec = &rx_exec, .prach_exec = dedicated ? &prach_exec : nullptr});

    std::unique_ptr<ru_sdr_executor_mapper> mapper = create_ru_sdr_executor_mapper(config);
    ASSERT_NE(mapper, nullptr);
    task_executor& expected = dedicated ? static_cast<task_executor&>(prach_exec) : high_prio_exec;
    EXPECT_EQ(&mapper->get_sector_mapper(0).prach_executor(), &expected) << "dedicated=" << dedicated;
    EXPECT_EQ(&mapper->get_sector_mapper(0).downlink_executor(), &high_prio_exec);
  }
}

TEST_F(RuSdrExecutorMapperFixture, triple_profile_uses_the_dedicated_prach_executor)
{
  for (bool dedicated : {false, true}) {
    ru_sdr_executor_mapper_triple_configuration config = {};
    config.radio_exec                                  = &radio_exec;
    config.high_prio_executor                          = &high_prio_exec;
    config.baseband_exec.push_back({.tx_exec    = &tx_exec,
                                    .rx_exec    = &rx_exec,
                                    .ul_exec    = &ul_exec,
                                    .prach_exec = dedicated ? &prach_exec : nullptr});

    std::unique_ptr<ru_sdr_executor_mapper> mapper = create_ru_sdr_executor_mapper(config);
    ASSERT_NE(mapper, nullptr);
    task_executor& expected = dedicated ? static_cast<task_executor&>(prach_exec) : high_prio_exec;
    EXPECT_EQ(&mapper->get_sector_mapper(0).prach_executor(), &expected) << "dedicated=" << dedicated;
    EXPECT_EQ(&mapper->get_sector_mapper(0).uplink_executor(), &ul_exec);
  }
}
