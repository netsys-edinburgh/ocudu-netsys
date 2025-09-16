/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "external_ul_processor_example_impl.h"
#include "srsran/phy/support/resource_grid_reader.h"
#include "srsran/phy/support/resource_grid_writer.h"

using namespace srsran;

void external_ul_processor_example_impl::process(resource_grid_writer&       grid_writer,
                                                 const resource_grid_reader& grid_reader,
                                                 slot_point                  slot,
                                                 unsigned                    symbol)
{
  // Save the resource grid.
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    // Copy the symbols into the temporary buffer.
    grid_reader.get(temp_buffer, i_port, symbol, 0);

    // [EXTERNAL CODE INSERTION START] Test your DSP processing here.

    for (auto& i_re : temp_buffer) {
      // Dummy processing: scale the resource elements by 0.1. This offsets the console RSRP measurements by 20 dB.
      i_re *= 0.1f;
    }

    // [EXTERNAL CODE INSERTION END]

    // Write the processed symbols back to the resource grid.
    grid_writer.put(i_port, symbol, 0, temp_buffer);
  }
}
