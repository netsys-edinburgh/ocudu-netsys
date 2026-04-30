// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "tap_ul_resource_grid_epre_zmq.h"
#include "ocudu/ocuduvec/modulus_square.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"

using namespace ocudu;

void tap_ul_resource_grid_epre_zmq::compute_epre(const resource_grid_reader& grid_reader)
{
  auto prb_epre_buffer = temp_buffers.get();
  if (!prb_epre_buffer) {
    logger.warning("Failed to get temporary buffer.");
    return;
  }

  // Prepare buffer size.
  prb_epre_buffer->resize(nof_subc);

  // Calculate the accumulated energy per subcarrier.
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
      if ((i_symbol == 0) && (i_port == 0)) {
        ocuduvec::modulus_square(*prb_epre_buffer, grid_reader.get_view(i_port, i_symbol));
      } else {
        ocuduvec::modulus_square_and_add(*prb_epre_buffer, grid_reader.get_view(i_port, i_symbol), *prb_epre_buffer);
      }
    }
  }

  bool success = deps->get_executor().defer(
      [this, buff = std::move(prb_epre_buffer)]() { deps->get_backend().send_buffer(*buff); });
  if (!success) {
    logger.warning("Failed to defer send buffer task.");
  }
}

void tap_ul_resource_grid_epre_zmq::process(resource_grid_writer&                                     grid_writer,
                                            const resource_grid_reader&                               grid_reader,
                                            slot_point                                                slot,
                                            unsigned                                                  symbol,
                                            span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                                            span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                                            span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                                            span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus)
{
  // Get resource grid dimensions.
  nof_symbols           = grid_reader.get_nof_symbols();
  nof_ports             = grid_reader.get_nof_ports();
  nof_subc              = grid_reader.get_nof_subc();
  last_processed_symbol = symbol;

  // Invoke base instance processing if present.
  if (base_instance) {
    base_instance->process(grid_writer, grid_reader, slot, symbol, pusch_pdus, pucch_pdus, pucch_f1_pdus, srs_pdus);
  }

  // Process EPRE if it is the last received symbol.
  if (symbol == nof_symbols - 1) {
    compute_epre(grid_reader);
  }
}

void tap_ul_resource_grid_epre_zmq::process_quiet(const resource_grid_reader& grid_reader, slot_point slot)
{
  // Invoke base instance processing if present.
  if (base_instance) {
    base_instance->process_quiet(grid_reader, slot);
  }

  // Process EPRE.
  compute_epre(grid_reader);
}

void tap_ul_resource_grid_epre_zmq::process_prach(prach_buffer& buffer, const prach_buffer_context& context)
{
  // Invoke base instance processing if present.
  if (base_instance) {
    base_instance->process_prach(buffer, context);
  }
}
