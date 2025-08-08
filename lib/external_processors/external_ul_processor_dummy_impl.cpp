/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "external_ul_processor_dummy_impl.h"
#include "srsran/phy/support/resource_grid_reader.h"
#include "srsran/phy/support/resource_grid_writer.h"

using namespace srsran;

void external_ul_processor_dummy_impl::process(const upper_phy_rx_symbol_context& context,
                                               const shared_resource_grid&        grid)
{
  // If the grid does not contain the number of symbols we intend to process, the processing is skipped.
  if ((context.symbol != (nof_symbols - 1))) {
    return;
  }

  // Obtain a resource grid reader. This is used to access the resource grid data.
  const resource_grid_reader& rg_reader = grid.get_reader();
  // Obtain a resource grid writer. This is used to write the processed data back into the resource grid.
  resource_grid_writer& rg_writer = grid.get_writer();

  // Save the resource grid.
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
      // Copy the symbols into the temporary buffer.
      rg_reader.get(temp_buffer, i_port, i_symbol, 0);

      // [EXTERNAL CODE INSERTION START] Test your DSP processing here.

      for (auto& i_re : temp_buffer) {
        // Dummy processing: scale the resource elements by 0.1. This offsets the console RSRP measurements by 20 dB.
        i_re *= 0.1f;
      }

      // [EXTERNAL CODE INSERTION END]

      // Write the processed symbols back to the resource grid.
      rg_writer.put(i_port, i_symbol, 0, temp_buffer);
    }
  }
}
