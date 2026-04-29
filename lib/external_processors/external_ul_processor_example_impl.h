// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "external_ul_processor.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/prach/prach_constants.h"
#include <regex>

namespace ocudu {

/// Example implementation of an external uplink processor with trivial processing.
class external_ul_processor_example_impl : public external_ul_processor
{
public:
  /// Number of symbols in a slot. Used for quiet slot processing.
  static constexpr unsigned nof_slot_symbols = MAX_NSYMB_PER_SLOT;

  /// \brief Constructor that initializes the external processor.
  ///
  /// \param[in] nof_rb Number of resource blocks in the resource grid.
  /// \param[in] nof_ports_ Number of ports to process.
  /// \param[in] processor_arguments custom arguments for the processor.
  external_ul_processor_example_impl(unsigned nof_rb, unsigned nof_ports_, const std::string& processor_arguments) :
    temp_buffer(nof_rb * NOF_SUBCARRIERS_PER_RB),
    nof_ports(nof_ports_),
    logger(ocudulog::fetch_basic_logger("PHY_TAP", true))
  {
    ocudulog::basic_levels log_level = ocudulog::basic_levels::info;

    std::smatch match;
    std::regex  log_level_regex(R"(log_level=([a-zA-Z]{1,}))");
    bool        has_log_level = std::regex_search(processor_arguments, match, log_level_regex);
    if (has_log_level) {
      std::optional<ocudulog::basic_levels> found_log_level = ocudulog::str_to_basic_level(match[1].str());
      if (found_log_level) {
        log_level = *found_log_level;
      } else {
        fmt::print("Invalid log level '{}' for the external UL processor plugin.", match[1].str());
      }
    }

    logger.set_level(log_level);

    std::regex enable_quiet_processing_regex(R"(enable_quiet_processing=([a-zA-Z]{1,}))");
    bool has_enable_quiet_processing = std::regex_search(processor_arguments, match, enable_quiet_processing_regex);
    if (has_enable_quiet_processing) {
      std::string parsed_text = match[1].str();
      std::transform(parsed_text.begin(), parsed_text.end(), parsed_text.begin(), ::tolower);
      if (parsed_text == "true") {
        enable_quiet_processing = true;
      }
    }
  }

  // See the interface for documentation.
  void process(resource_grid_writer&                                     grid_writer,
               const resource_grid_reader&                               grid_reader,
               slot_point                                                slot,
               unsigned                                                  symbol,
               span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
               span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
               span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
               span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) override;

  // See the interface for documentation.
  void process_quiet(const resource_grid_reader& grid_reader, slot_point slot) override;

  // See the interface for documentation.
  void process_prach(prach_buffer& buffer, const prach_buffer_context& context) override;

  /// Buffer for the temporary storage of the resource grid data.
  std::vector<cf_t> temp_buffer;
  /// Buffer for the temporary storage of the PRACH data.
  static_vector<cf_t, prach_constants::LONG_SEQUENCE_LENGTH> temp_buffer_prach;
  /// Number of ports to process.
  unsigned nof_ports;
  /// Index of the last processed symbol.
  unsigned last_processed_symbol = 0;
  /// Enables or disables the processing of quiet UL symbols.
  bool enable_quiet_processing = false;
  /// Logger object.
  ocudulog::basic_logger& logger;
};

} // namespace ocudu
