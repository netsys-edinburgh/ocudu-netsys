// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ofh_message_receiver_impl.h"
#include "../support/ofh_ul_rx_diagnostics.h"
#include "ofh_rx_window_checker.h"
#include "ocudu/adt/format.h"
#include "ocudu/instrumentation/traces/ofh_traces.h"
#include <ctime>

using namespace ocudu;
using namespace ofh;

/// Upper bound of the arrival age correction, in symbols. A larger age means a clock step between the kernel timestamp
/// and the processing; the correction is then not applied.
static constexpr uint64_t max_arrival_age_symbols = 1000;

/// Returns CLOCK_REALTIME in nanoseconds, the clock of the kernel receive timestamps.
static uint64_t realtime_now_ns()
{
  ::timespec ts = {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

message_receiver_impl::message_receiver_impl(const message_receiver_config&  config,
                                             message_receiver_dependencies&& dependencies) :
  logger(*dependencies.logger),
  sector_id(config.sector),
  nof_symbols(config.nof_symbols),
  scs(config.scs),
  symbol_duration_ns(1000000ULL / (get_nof_slots_per_subframe(config.scs) * config.nof_symbols)),
  vlan_params(config.vlan_params),
  ul_prach_eaxc(config.prach_eaxc),
  ul_eaxc(config.ul_eaxc),
  warn_unreceived_frames_on_first_rx_message(config.warn_unreceived_frames ==
                                             warn_unreceived_ru_frames::after_traffic_detection),
  window_checker(*dependencies.window_checker),
  window_handler(*dependencies.window_handler),
  seq_id_checker(std::move(dependencies.seq_id_checker)),
  vlan_decoder(std::move(dependencies.eth_frame_decoder)),
  ecpri_decoder(std::move(dependencies.ecpri_decoder)),
  data_flow_uplink(std::move(dependencies.data_flow_uplink)),
  data_flow_prach(std::move(dependencies.data_flow_prach)),
  metrics_collector(config.are_metrics_enabled,
                    data_flow_uplink->get_metrics_collector(),
                    data_flow_prach->get_metrics_collector())
{
  ocudu_assert(vlan_decoder, "Invalid VLAN decoder");
  ocudu_assert(ecpri_decoder, "Invalid eCPRI decoder");
  ocudu_assert(data_flow_uplink, "Invalid uplink IQ data flow");
  ocudu_assert(data_flow_prach, "Invalid uplink PRACH IQ data flow");
  ocudu_assert(seq_id_checker, "Invalid sequence id checker");

  ul_rx_diagnostics::get().set_symbol_duration(std::chrono::nanoseconds(symbol_duration_ns));
}

void message_receiver_impl::on_new_frame(ether::unique_rx_buffer buffer)
{
  process_new_frame(std::move(buffer));
}

void message_receiver_impl::process_new_frame(ether::unique_rx_buffer buffer)
{
  span<const uint8_t> payload = buffer.data();

  // Time the frame spent between the kernel and this point (socket queue, receive thread, executor queue).
  const uint64_t kernel_rx_ns        = buffer.get_kernel_rx_timestamp_ns();
  const uint64_t read_ns             = buffer.get_read_timestamp_ns();
  const uint64_t now_ns              = ((kernel_rx_ns != 0) || (read_ns != 0)) ? realtime_now_ns() : 0;
  unsigned       arrival_age_symbols = 0;
  int64_t        read_to_process_ns  = -1;
  if ((kernel_rx_ns != 0) && (now_ns > kernel_rx_ns)) {
    const uint64_t age_symbols = (now_ns - kernel_rx_ns) / symbol_duration_ns;
    arrival_age_symbols        = (age_symbols <= max_arrival_age_symbols) ? static_cast<unsigned>(age_symbols) : 0;
  }
  if ((read_ns != 0) && (now_ns > read_ns)) {
    read_to_process_ns = static_cast<int64_t>(now_ns - read_ns);
  }

  // Creates and starts the execution time measurer.
  time_execution_measurer meas(metrics_collector.enabled());

  ether::vlan_frame_params eth_params;
  span<const uint8_t>      ecpri_pdu = vlan_decoder->decode(payload, eth_params);
  if (OCUDU_UNLIKELY(ecpri_pdu.empty() || should_ethernet_frame_be_filtered(eth_params))) {
    return;
  }

  ecpri::packet_parameters ecpri_params;
  span<const uint8_t>      ofh_pdu = ecpri_decoder->decode(ecpri_pdu, ecpri_params);
  if (OCUDU_UNLIKELY(ofh_pdu.empty() || should_ecpri_packet_be_filtered(ecpri_params))) {
    metrics_collector.increase_ecpri_corrupted_messages();
    return;
  }

  // Command the rx windown handler to start logging unreceived RU frames.
  if (OCUDU_UNLIKELY(warn_unreceived_frames_on_first_rx_message)) {
    window_handler.start_logging_unreceived_messages();
    warn_unreceived_frames_on_first_rx_message = false;
  }

  // Verify the sequence identifier.
  const ecpri::iq_data_parameters& ecpri_iq_params = std::get<ecpri::iq_data_parameters>(ecpri_params.type_params);
  unsigned                         eaxc            = ecpri_iq_params.pc_id;
  int nof_skipped_seq_id = seq_id_checker->update_and_compare_seq_id(eaxc, (ecpri_iq_params.seq_id >> 8));
  // A message is from the past. It is not dropped, as it may have been delivered out of order and still fall inside the
  // reception window.
  if (OCUDU_UNLIKELY(nof_skipped_seq_id < 0)) {
    metrics_collector.increase_past_seq_id_messages();

    logger.info("Sector#{}: Open Fronthaul receiver detected User-Plane packet for eAxC value '{}' with an eCPRI "
                "sequence identifier from the past",
                sector_id,
                eaxc);
  } else if (OCUDU_UNLIKELY(nof_skipped_seq_id > 0)) {
    metrics_collector.update_future_seq_id_messages(nof_skipped_seq_id);

    // A jump in sequence identifiers can be a result of out of order delivery, meaning that the skipped identifiers are
    // only potentially lost - they can be received later on.
    logger.info("Sector#{}: potentially lost '{}' Open Fronthaul User-Plane packets sent by the RU for eAxC value '{}'",
                sector_id,
                nof_skipped_seq_id,
                eaxc);
  }

  std::optional<slot_symbol_point> slot_point = uplane_peeker::peek_slot_symbol_point(ofh_pdu, nof_symbols, scs);
  if (OCUDU_UNLIKELY(!slot_point)) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane packet as the slot could not be peeked",
                sector_id);
    return;
  }

  // Fill the reception window statistics, evaluated at the kernel arrival time of the frame.
  std::optional<int> processing_offset = window_checker.update_rx_window_statistics(*slot_point, arrival_age_symbols);
  if (processing_offset) {
    ul_rx_diagnostics::get().on_frame_processed(
        read_to_process_ns, *processing_offset - static_cast<int>(arrival_age_symbols), *processing_offset);
  }

  // Peek the filter index and check that it is valid.
  std::optional<filter_index_type> filter_type = uplane_peeker::peek_filter_index(ofh_pdu);
  if (OCUDU_UNLIKELY(!filter_type)) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane message as the filter index could not be peeked",
                sector_id);
    return;
  }

  trace_point decode_tp = ofh_tracer.now();
  if (is_a_prach_message(*filter_type)) {
    data_flow_prach->decode_type1_message(eaxc, ofh_pdu, nof_skipped_seq_id == 0);
    metrics_collector.update_prach_stats(meas.stop());
    ofh_tracer << trace_event("ofh_receiver_decode_prach", decode_tp);
    return;
  }

  data_flow_uplink->decode_type1_message(eaxc, ofh_pdu, nof_skipped_seq_id == 0);
  metrics_collector.update_uplink_stats(meas.stop());
  ofh_tracer << trace_event("ofh_receiver_decode_data", decode_tp);
}

bool message_receiver_impl::should_ecpri_packet_be_filtered(const ecpri::packet_parameters& ecpri_params) const
{
  if (OCUDU_UNLIKELY(ecpri_params.header.msg_type != ecpri::message_type::iq_data)) {
    logger.info(
        "Sector#{}: dropped received Open Fronthaul User-Plane packet as decoded eCPRI message type is not for IQ data",
        sector_id);

    return true;
  }

  const auto& ecpri_iq_params = std::get<ecpri::iq_data_parameters>(ecpri_params.type_params);
  if (OCUDU_UNLIKELY(
          (std::find(ul_eaxc.begin(), ul_eaxc.end(), ecpri_iq_params.pc_id) == ul_eaxc.end()) &&
          (std::find(ul_prach_eaxc.begin(), ul_prach_eaxc.end(), ecpri_iq_params.pc_id) == ul_prach_eaxc.end()))) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane packet as decoded eAxC value '{}' is not "
                "configured in reception",
                sector_id,
                ecpri_iq_params.pc_id);

    return true;
  }

  return false;
}

bool message_receiver_impl::should_ethernet_frame_be_filtered(const ether::vlan_frame_params& eth_params) const
{
  if (OCUDU_UNLIKELY(eth_params.mac_src_address != vlan_params.mac_src_address)) {
    logger.debug("Sector#{}: dropped received Ethernet frame as source MAC addresses do not match (detected={:02X}, "
                 "expected={:02X})",
                 sector_id,
                 fmt::join(eth_params.mac_src_address, ":"),
                 fmt::join(vlan_params.mac_src_address, ":"));

    return true;
  }

  if (OCUDU_UNLIKELY(eth_params.mac_dst_address != vlan_params.mac_dst_address)) {
    logger.debug(
        "Sector#{}: dropped received Ethernet frame as destination MAC addresses do not match match (detected={:02X}, "
        "expected={:02X})",
        sector_id,
        fmt::join(eth_params.mac_dst_address, ":"),
        fmt::join(vlan_params.mac_dst_address, ":"));

    return true;
  }

  if (OCUDU_UNLIKELY(eth_params.eth_type != vlan_params.eth_type)) {
    logger.info("Sector#{}: dropped received Ethernet frame as decoded Ethernet type is '{}' but expected '{}'",
                sector_id,
                eth_params.eth_type,
                vlan_params.eth_type);

    return true;
  }

  return false;
}
