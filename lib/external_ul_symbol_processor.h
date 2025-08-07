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

#include "srsran/phy/support/resource_grid_reader.h"
#include "srsran/phy/support/resource_grid_writer.h"
#include "srsran/phy/support/shared_resource_grid.h"
#include "srsran/phy/upper/upper_phy_rx_symbol_handler.h"
#include "srsran/ran/cyclic_prefix.h"

namespace srsran {

class external_ul_symbol_processor
{
public:
  // Number of symbols to process at once. For now, the entire slot is processed.
  static constexpr unsigned nof_symbols = MAX_NSYMB_PER_SLOT;

  /// \brief Constructor that initializes the external processor.
  ///
  /// \param[in] nof_rb Number of resource blocks in the resource grid.
  /// \param[in] nof_ports_ Number of ports to process.
  /// \param[in] processor_arguments custom arguments for the processor.
  explicit external_ul_symbol_processor(unsigned nof_rb, unsigned nof_ports_, const std::string& processor_arguments) :
    temp_buffer(nof_rb * NRE), nof_ports(nof_ports_)
  {
  }

  /// \brief Processes the UL symbols in the resource grid.
  ///
  /// This method is where the actual processing of the UL symbols takes place. Any external DSP processing must be
  /// called from this method.
  ///
  /// \param[in] context The context of the received UL symbol, including sector, slot, and symbol index.
  /// \param[in] grid The resource grid containing the UL symbols to process.
  void process(const upper_phy_rx_symbol_context& context, const shared_resource_grid& grid)
  {
    // If the grid does not contain the number of symbols we intend to process, the processing is skipped.
    if ((context.symbol != (nof_symbols - 1))) {
      return;
    }

    // Obtain a resource grid rader. This is used to access the resource grid data.
    const resource_grid_reader& rg_reader = grid.get_reader();
    // Obtain a resource grid writer. This is used to write the processed data back into the resource grid.
    resource_grid_writer& rg_writer = grid.get_writer();

    // Save the resource grid.
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
        // Copy the symbols into the temporary buffer.
        rg_reader.get(temp_buffer, i_port, i_symbol, 0);

        // Add your custom DSP processing here (replace the dummy example below with actual processing).
        for (auto& i_re : temp_buffer) {
          // Dummy processing: scale the resource elements by 0.1.
          i_re *= 0.1f;
        }

        // Write the processed symbols back to the resource grid.
        rg_writer.put(i_port, i_symbol, 0, temp_buffer);
      }
    }
  }

  /// Buffer for the temporary storage of the resource grid data.
  std::vector<cf_t> temp_buffer;

  /// Number of ports to process.
  unsigned nof_ports;
};

} // namespace srsran
