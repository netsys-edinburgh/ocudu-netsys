// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "external_ul_processor_example_impl.h"
#include "ocudu/adt/bounded_integer.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/fill.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/ran/prach/prach_preamble_information.h"

using namespace ocudu;

void external_ul_processor_example_impl::process(
    resource_grid_writer&                                     grid_writer,
    const resource_grid_reader&                               grid_reader,
    slot_point                                                slot,
    unsigned                                                  symbol,
    span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
    span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
    span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
    span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus)
{
  // Log the slot and symbol being processed.
  logger.debug("Processing symbol: slot={}, symbol={}", slot, symbol);
  last_processed_symbol = symbol;

  // Log each received PDU.
  for (const auto& pusch_pdu : pusch_pdus) {
    const auto& pdu = pusch_pdu.pdu;
    logger.debug("  PUSCH PDU: rnti={}, symb=[{}, {})",
                 pdu.rnti,
                 pdu.start_symbol_index,
                 pdu.start_symbol_index + pdu.nof_symbols);
  }

  for (const auto& pucch_pdu : pucch_pdus) {
    logger.debug("  PUCCH PDU: rnti={}, format={})", pucch_pdu.context.rnti, to_string(pucch_pdu.context.format));
  }

  for (const auto& pucch_f1_pdu : pucch_f1_pdus) {
    logger.debug("  PUCCH Format 1 PDU: symb=[{}, {})",
                 pucch_f1_pdu.start_symbol_index,
                 pucch_f1_pdu.start_symbol_index + pucch_f1_pdu.nof_symbols);
  }

  for (const auto& srs_pdu : srs_pdus) {
    logger.debug("  SRS PDU: rnti {}, symb=[{}, {})",
                 srs_pdu.context.rnti,
                 srs_pdu.config.resource.start_symbol,
                 srs_pdu.config.resource.start_symbol.value() +
                     static_cast<unsigned>(srs_pdu.config.resource.nof_symbols));
  }

  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    // Copy the symbols into the temporary buffer.
    grid_reader.get(temp_buffer, i_port, symbol, 0);

    // [EXTERNAL CODE INSERTION START] Insert your DSP processing here.

    // Dummy processing: scale the resource elements by 0.1. This offsets the console RSRP measurements by 20 dB.
    ocuduvec::sc_prod(temp_buffer, temp_buffer, 0.1f);

    // [EXTERNAL CODE INSERTION END]

    // Write the processed symbols back to the resource grid.
    grid_writer.put(i_port, symbol, 0, temp_buffer);
  }
}

void external_ul_processor_example_impl::process_quiet(const resource_grid_reader& grid_reader, slot_point slot)
{
  // Processing of unallocated UL symbols needs to be explicitly requested.
  if (!enable_quiet_processing) {
    return;
  }

  // Update the symbol index. Note that the resulting index is an estimation based on the processing of allocated
  // symbols.
  if (last_processed_symbol == nof_slot_symbols - 1) {
    last_processed_symbol = 0;
  } else if (last_processed_symbol > 0) {
    ++last_processed_symbol;
  }

  for (unsigned symbol_ix = last_processed_symbol; symbol_ix != nof_slot_symbols; ++symbol_ix) {
    // Log the slot and symbol being processed.
    logger.debug("Processing unallocated symbol: slot={}, symbol={}", slot, symbol_ix);

    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      // Forced zeroing of all non-UL symbols in the flexible slot.
      if (tdd_pattern1.has_value() &&
          ((((slot.slot_index() % tdd_pattern1->periodicity_slots) == tdd_pattern1->flexible_slot_idx) &&
            (symbol_ix < tdd_pattern1->first_ul_symbol_idx)) ||
           (tdd_pattern2.has_value() &&
            ((slot.slot_index() % tdd_pattern2->periodicity_slots) == tdd_pattern2->flexible_slot_idx) &&
            (symbol_ix < tdd_pattern2->first_ul_symbol_idx)))) {
        ocuduvec::fill(temp_buffer, {0.0f, 0.0f});
      } else {
        // Copy the symbols into the temporary buffer.
        grid_reader.get(temp_buffer, i_port, symbol_ix, 0);
      }

      // [EXTERNAL CODE INSERTION START] Insert your DSP processing here.

      // Dummy processing: scale the resource elements by 0.1. This offsets the console RSRP measurements by 20 dB.
      ocuduvec::sc_prod(temp_buffer, temp_buffer, 0.1f);

      // [EXTERNAL CODE INSERTION END]
    }
  }

  // Update the symbol index at the end of the current slot.
  last_processed_symbol = nof_slot_symbols - 1;
}

void external_ul_processor_example_impl::process_prach(prach_buffer& buffer, const prach_buffer_context& context)
{
  // Log the PRACH occasion being processed.
  logger.debug("Processing PRACH: slot={}, format={}", context.slot, to_string(context.format));

  // Obtain the preamble information. This includes useful parameters such as the number of OFDM symbols containing
  // PRACH for each time domain occasion.
  prach_preamble_information preamble_info =
      is_long_preamble(context.format)
          ? get_prach_preamble_long_info(context.format)
          : get_prach_preamble_short_info(context.format, to_ra_subcarrier_spacing(context.pusch_scs), true);

  // Iterate over all receive ports, PRACH symbols, time domain and frequency domain occasions,
  for (unsigned i_port = 0, i_port_end = context.ports.size(); i_port != i_port_end; ++i_port) {
    for (unsigned i_td_occasion = 0; i_td_occasion != context.nof_td_occasions; ++i_td_occasion) {
      for (unsigned i_fd_occasion = 0; i_fd_occasion != context.nof_fd_occasions; ++i_fd_occasion) {
        for (unsigned i_symbol = 0; i_symbol != preamble_info.nof_symbols; ++i_symbol) {
          // Get a view over the PRACH symbols.
          span<cbf16_t> preamble = buffer.get_symbol(i_port, i_td_occasion, i_fd_occasion, i_symbol);

          // Convert the PRACH samples from BF16 into float.
          temp_buffer_prach.resize(preamble.size());
          ocuduvec::convert(temp_buffer_prach, preamble);

          // [EXTERNAL CODE INSERTION START] Insert your DSP processing here.

          // Dummy processing: scale the PRACH symbols by 0.1. This offsets the log RSSI measurements by 20 dB.
          ocuduvec::sc_prod(temp_buffer_prach, temp_buffer_prach, 0.1f);

          // [EXTERNAL CODE INSERTION END]

          // Convert to BF16 and write the processed PRACH symbols into the source buffer.
          ocuduvec::convert(preamble, temp_buffer_prach);
        }
      }
    }
  }
}
