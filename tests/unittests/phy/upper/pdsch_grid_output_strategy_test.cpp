// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Unit tests for PDSCH CUDA grid output strategy selection.

#include "../support/resource_grid_test_doubles.h"
#include "cuda/pdsch_device_grid_writer_cuda.h"
#include "pdsch_grid_output_strategy.h"
#include "resource_grid_cuda_visible_impl.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/support/support_factories.h"
#include <cstdlib>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>

using namespace ocudu;

namespace {

class scoped_env_var
{
public:
  explicit scoped_env_var(const char* name_) : name(name_)
  {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
      original_value = value;
    }
  }

  ~scoped_env_var()
  {
    if (original_value.has_value()) {
      (void)setenv(name.c_str(), original_value->c_str(), 1);
    } else {
      (void)unsetenv(name.c_str());
    }
  }

  bool set(const char* value) { return setenv(name.c_str(), value, 1) == 0; }

  bool unset() { return unsetenv(name.c_str()) == 0; }

private:
  std::string                name;
  std::optional<std::string> original_value;
};

testing::AssertionResult cuda_runtime_available()
{
  int         device_count = 0;
  cudaError_t status       = cudaGetDeviceCount(&device_count);
  if ((status != cudaSuccess) || (device_count == 0)) {
    return testing::AssertionFailure() << "CUDA runtime device is not available.";
  }
  return testing::AssertionSuccess();
}

resource_grid_context make_context()
{
  return {.slot = slot_point(0, 0), .sector = 0};
}

ocudulog::basic_logger& test_logger()
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("TEST", false);
  logger.set_level(ocudulog::basic_levels::none);
  return logger;
}

} // namespace

TEST(pdsch_grid_output_strategy_test, returns_host_writer_when_device_grid_writer_is_disabled)
{
  std::unique_ptr<resource_grid> grid = create_resource_grid_factory()->create(1, 14, 24);
  ASSERT_NE(grid, nullptr);

  shared_resource_grid_spy shared_grid_spy(*grid);
  shared_resource_grid     shared_grid = shared_grid_spy.get_grid();
  resource_grid_writer*    host_writer = &shared_grid.get_writer();

  std::unique_ptr<pdsch_grid_output_strategy> strategy = create_pdsch_grid_output_strategy(test_logger(), false);
  ASSERT_NE(strategy, nullptr);
  strategy->configure(make_context(), shared_grid);

  EXPECT_EQ(&strategy->get_pdsch_writer(shared_grid), host_writer);
  strategy->before_send_grid();
}

TEST(pdsch_grid_output_strategy_test, uses_sidecar_device_writer_for_host_resource_grid)
{
  testing::AssertionResult cuda_available = cuda_runtime_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  std::unique_ptr<resource_grid> grid = create_resource_grid_factory()->create(1, 14, 24);
  ASSERT_NE(grid, nullptr);

  shared_resource_grid_spy shared_grid_spy(*grid);
  shared_resource_grid     shared_grid = shared_grid_spy.get_grid();
  resource_grid_writer*    host_writer = &shared_grid.get_writer();

  std::unique_ptr<pdsch_grid_output_strategy> strategy = create_pdsch_grid_output_strategy(test_logger(), true);
  ASSERT_NE(strategy, nullptr);
  strategy->configure(make_context(), shared_grid);

  resource_grid_writer& selected_writer = strategy->get_pdsch_writer(shared_grid);
  EXPECT_NE(&selected_writer, host_writer);
  EXPECT_TRUE(selected_writer.supports_device_grid_mapping());
  strategy->before_send_grid();
}

TEST(pdsch_grid_output_strategy_test, uses_direct_writer_for_cuda_visible_grid_by_default)
{
  testing::AssertionResult cuda_available = cuda_runtime_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var direct_grid("OCUDU_PDSCH_DIRECT_DEVICE_GRID");
  ASSERT_TRUE(direct_grid.unset());

  cuda_visible_resource_grid grid(1, 14, 24);
  if (!grid.is_valid()) {
    GTEST_SKIP() << "CUDA-visible resource grid is not available.";
  }

  shared_resource_grid_spy shared_grid_spy(grid);
  shared_resource_grid     shared_grid = shared_grid_spy.get_grid();
  resource_grid_writer*    host_writer = &shared_grid.get_writer();
  ASSERT_TRUE(host_writer->supports_device_grid_mapping());

  std::unique_ptr<pdsch_grid_output_strategy> strategy = create_pdsch_grid_output_strategy(test_logger(), true);
  ASSERT_NE(strategy, nullptr);
  strategy->configure(make_context(), shared_grid);

  EXPECT_EQ(&strategy->get_pdsch_writer(shared_grid), host_writer);
  strategy->before_send_grid();
}

TEST(pdsch_grid_output_strategy_test, direct_cuda_visible_grid_path_can_be_disabled)
{
  testing::AssertionResult cuda_available = cuda_runtime_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var direct_grid("OCUDU_PDSCH_DIRECT_DEVICE_GRID");
  ASSERT_TRUE(direct_grid.set("0"));

  cuda_visible_resource_grid grid(1, 14, 24);
  if (!grid.is_valid()) {
    GTEST_SKIP() << "CUDA-visible resource grid is not available.";
  }

  shared_resource_grid_spy shared_grid_spy(grid);
  shared_resource_grid     shared_grid = shared_grid_spy.get_grid();
  resource_grid_writer*    host_writer = &shared_grid.get_writer();
  ASSERT_TRUE(host_writer->supports_device_grid_mapping());

  std::unique_ptr<pdsch_grid_output_strategy> strategy = create_pdsch_grid_output_strategy(test_logger(), true);
  ASSERT_NE(strategy, nullptr);
  strategy->configure(make_context(), shared_grid);

  resource_grid_writer& selected_writer = strategy->get_pdsch_writer(shared_grid);
  EXPECT_NE(&selected_writer, host_writer);
  EXPECT_TRUE(selected_writer.supports_device_grid_mapping());
  strategy->before_send_grid();
}

// The sidecar device writer is allocated once and rebound to each slot grid. A slot must still get only its own
// mapping: the device grid is cleared between slots, and materialization targets the current slot grid.
TEST(pdsch_grid_output_strategy_test, sidecar_device_writer_is_reused_and_maps_each_slot_into_its_own_grid)
{
  testing::AssertionResult cuda_available = cuda_runtime_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  static constexpr unsigned nof_ports   = 1;
  static constexpr unsigned nof_symbols = 14;
  static constexpr unsigned nof_subc    = 24;

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  std::unique_ptr<resource_grid>         first_grid   = grid_factory->create(nof_ports, nof_symbols, nof_subc);
  std::unique_ptr<resource_grid>         second_grid  = grid_factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(first_grid, nullptr);
  ASSERT_NE(second_grid, nullptr);
  first_grid->set_all_zero();
  second_grid->set_all_zero();

  shared_resource_grid_spy first_spy(*first_grid);
  shared_resource_grid_spy second_spy(*second_grid);
  shared_resource_grid     first_slot  = first_spy.get_grid();
  shared_resource_grid     second_slot = second_spy.get_grid();

  std::unique_ptr<pdsch_grid_output_strategy> strategy = create_pdsch_grid_output_strategy(test_logger(), true);
  ASSERT_NE(strategy, nullptr);

  // Maps one RE through the device grid of the selected writer, as the device mapper does.
  auto map_one_re = [](resource_grid_writer& writer, unsigned symbol, unsigned subc, cf_t value) {
    ASSERT_TRUE(writer.prepare_device_grid_mapping(nullptr));
    auto*   device_grid = static_cast<cbf16_t*>(writer.get_device_grid_bf16());
    cbf16_t bf16_value  = to_cbf16(value);
    ASSERT_EQ(
        cudaMemcpy(device_grid + symbol * nof_subc + subc, &bf16_value, sizeof(bf16_value), cudaMemcpyHostToDevice),
        cudaSuccess);
    ASSERT_TRUE(writer.on_device_grid_mapping_enqueued(nullptr));
  };
  auto read_re = [](resource_grid& grid, unsigned symbol, unsigned subc) {
    return to_cf(grid.get_reader().get_view(0, symbol)[subc]);
  };

  const cf_t     first_value(1.0F, 2.0F);
  const cf_t     second_value(3.0F, 4.0F);
  const unsigned nof_allocations = pdsch_device_grid_writer_cuda::get_nof_device_grid_allocations();

  strategy->configure(make_context(), first_slot);
  resource_grid_writer& first_writer      = strategy->get_pdsch_writer(first_slot);
  void*                 first_device_grid = first_writer.get_device_grid_bf16();
  map_one_re(first_writer, 2, 5, first_value);
  strategy->before_send_grid();

  strategy->configure(make_context(), second_slot);
  resource_grid_writer& second_writer = strategy->get_pdsch_writer(second_slot);
  EXPECT_EQ(&second_writer, &first_writer) << "the sidecar writer was created again for the second slot";
  EXPECT_EQ(second_writer.get_device_grid_bf16(), first_device_grid) << "the device grid was allocated again";
  map_one_re(second_writer, 7, 11, second_value);
  strategy->before_send_grid();

  // Addresses can repeat after a free, so the allocation count is what shows that the second slot reused the grid.
  EXPECT_EQ(pdsch_device_grid_writer_cuda::get_nof_device_grid_allocations(), nof_allocations + 1)
      << "the device grid was allocated for each slot";

  EXPECT_EQ(read_re(*first_grid, 2, 5), first_value);
  EXPECT_EQ(read_re(*first_grid, 7, 11), cf_t()) << "the second slot was written into the first slot grid";
  EXPECT_EQ(read_re(*second_grid, 7, 11), second_value);
  EXPECT_EQ(read_re(*second_grid, 2, 5), cf_t()) << "the first slot mapping leaked into the second slot grid";
}
