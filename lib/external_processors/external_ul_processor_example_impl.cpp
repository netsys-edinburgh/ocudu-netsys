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
#include "srsran/srsvec/sc_prod.h"

using namespace srsran;

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
    logger.debug("  PUSCH PDU: rnti={:#x}, symb=[{}, {})",
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
    srsvec::sc_prod(temp_buffer, temp_buffer, 0.1f);

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
      // Copy the symbols into the temporary buffer.
      grid_reader.get(temp_buffer, i_port, symbol_ix, 0);

      // [EXTERNAL CODE INSERTION START] Insert your DSP processing here.

      // Dummy processing: scale the resource elements by 0.1. This offsets the console RSRP measurements by 20 dB.
      srsvec::sc_prod(temp_buffer, temp_buffer, 0.1f);

      // [EXTERNAL CODE INSERTION END]
    }
  }
}
