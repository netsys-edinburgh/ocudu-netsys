/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "external_ul_processor.h"
#include "srsran/phy/upper/upper_phy_rx_symbol_handler.h"
#include "srsran/ran/cyclic_prefix.h"

namespace srsran {

class external_ul_processor_example_impl : public external_ul_processor
{
public:
  /// Number of symbols to process at once. For now, the entire slot is processed.
  static constexpr unsigned nof_symbols = MAX_NSYMB_PER_SLOT;

  /// \brief Constructor that initializes the external processor.
  ///
  /// \param[in] nof_rb Number of resource blocks in the resource grid.
  /// \param[in] nof_ports_ Number of ports to process.
  /// \param[in] processor_arguments custom arguments for the processor.
  explicit external_ul_processor_example_impl(unsigned           nof_rb,
                                              unsigned           nof_ports_,
                                              const std::string& processor_arguments) :
    temp_buffer(nof_rb * NRE), nof_ports(nof_ports_)
  {
  }

  // See the interface for documentation.
  void process(resource_grid_writer&       grid_writer,
               const resource_grid_reader& grid_reader,
               slot_point                  slot,
               unsigned                    symbol) override;

  /// Buffer for the temporary storage of the resource grid data.
  std::vector<cf_t> temp_buffer;

  /// Number of ports to process.
  unsigned nof_ports;
};

} // namespace srsran
