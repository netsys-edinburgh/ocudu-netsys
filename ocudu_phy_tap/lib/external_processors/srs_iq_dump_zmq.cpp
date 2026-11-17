// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "srs_iq_dump_zmq.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/subcarrier_spacing.h"

using namespace ocudu;

void srs_iq_dump_zmq::dump_occasion(const resource_grid_reader&                grid_reader,
                                    const uplink_pdu_slot_repository::srs_pdu& srs_pdu)
{
  const auto&    resource     = srs_pdu.config.resource;
  const auto&    ports        = srs_pdu.config.ports;
  const unsigned start_symbol = resource.start_symbol.value();
  const unsigned nof_symbols  = static_cast<unsigned>(resource.nof_symbols);
  const unsigned nof_ports    = ports.size();

  if ((nof_symbols == 0) || (nof_ports == 0)) {
    return;
  }
  if ((nof_symbols > max_srs_symbols) || (nof_ports > max_srs_ports)) {
    logger.warning("SRS occasion exceeds the IQ dump limits (nof_symbols={}, nof_ports={}); skipping dump.",
                   nof_symbols,
                   nof_ports);
    return;
  }

  // Derive the occasion's own SRS-carrying subcarriers (parameter M_sc_RS, after comb decimation) and their first
  // subcarrier, via the same helper ocudu's own SRS channel estimator uses for antenna port 0. This ensures the
  // dump only ever contains this UE's resource elements, not the whole resource grid.
  srs_information info     = get_srs_information(resource, /*i_antenna_port=*/0);
  const unsigned  nof_subc = info.sequence_length;

  // Grab a buffer from the pool. This, and the copy below, run synchronously on the RU execution context, but are
  // bounded, allocation-free operations, so they are safe to run here.
  auto buffer = deps->get_buffer_pool().get();
  if (!buffer) {
    logger.warning("Failed to get an IQ dump buffer; dropping this SRS occasion.");
    return;
  }

  // Copy the occasion's IQ samples out of the resource grid, symbol-major then port, matching the wire payload
  // layout documented in srs_iq_dump_header. The strided read (starting at the occasion's own first subcarrier,
  // stepping by the comb size) picks out only this UE's REs, skipping the interleaved REs that belong to other
  // comb offsets.
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      span<cf_t> dest(buffer->data() + (i_symbol * nof_ports + i_port) * nof_subc, nof_subc);
      grid_reader.get(dest, ports[i_port], start_symbol + i_symbol, info.mapping_initial_subcarrier, info.comb_size);
    }
  }

  srs_iq_dump_header header;
  header.rnti                       = static_cast<uint16_t>(srs_pdu.context.rnti);
  header.sfn                        = srs_pdu.context.slot.sfn();
  header.slot_index                 = static_cast<uint16_t>(srs_pdu.context.slot.slot_index());
  header.scs_khz                    = static_cast<uint16_t>(scs_to_khz(srs_pdu.context.slot.scs()));
  header.start_symbol               = static_cast<uint8_t>(start_symbol);
  header.nof_symbols                = static_cast<uint8_t>(nof_symbols);
  header.nof_ports                  = static_cast<uint8_t>(nof_ports);
  header.nof_subc                   = static_cast<uint16_t>(nof_subc);
  header.comb_size                  = static_cast<uint8_t>(resource.comb_size);
  header.comb_offset                = static_cast<uint8_t>(resource.comb_offset.value());
  header.cyclic_shift               = static_cast<uint8_t>(resource.cyclic_shift.value());
  header.sequence_id                = static_cast<uint16_t>(resource.sequence_id.value());
  header.configuration_index        = static_cast<uint8_t>(resource.configuration_index.value());
  header.bandwidth_index            = static_cast<uint8_t>(resource.bandwidth_index.value());
  header.freq_position              = static_cast<uint8_t>(resource.freq_position.value());
  header.freq_shift                 = static_cast<uint16_t>(resource.freq_shift.value());
  header.mapping_initial_subcarrier = static_cast<uint16_t>(info.mapping_initial_subcarrier);

  const unsigned nof_samples = nof_symbols * nof_ports * nof_subc;

  // Defer the actual send - the only unbounded-latency part of this operation - to the background worker thread.
  // The lambda owns the pool buffer, which is returned to the pool once the send completes.
  bool success = deps->get_executor().defer([this, header, buff = std::move(buffer), nof_samples]() {
    span<const uint8_t> header_bytes(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    deps->get_backend().send_header_and_payload(header_bytes, span<const cf_t>(buff->data(), nof_samples));
  });
  if (!success) {
    logger.warning("Failed to defer SRS IQ dump send task.");
  }
}

void srs_iq_dump_zmq::process(resource_grid_writer&                                     grid_writer,
                              const resource_grid_reader&                               grid_reader,
                              slot_point                                                slot,
                              unsigned                                                  symbol,
                              span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                              span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                              span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                              span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus)
{
  // Invoke base instance processing first.
  if (base_instance) {
    base_instance->process(grid_writer, grid_reader, slot, symbol, pusch_pdus, pucch_pdus, pucch_f1_pdus, srs_pdus);
  }

  // Dump every completed SRS occasion in this call.
  for (const auto& srs_pdu : srs_pdus) {
    dump_occasion(grid_reader, srs_pdu);
  }
}

void srs_iq_dump_zmq::process_quiet(const resource_grid_reader& grid_reader, slot_point slot)
{
  if (base_instance) {
    base_instance->process_quiet(grid_reader, slot);
  }
}

void srs_iq_dump_zmq::process_prach(prach_buffer& buffer, const prach_buffer_context& context)
{
  if (base_instance) {
    base_instance->process_prach(buffer, context);
  }
}
