// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "external_ul_processor.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/prach/prach_constants.h"
#include "ocudu/ran/tdd/tdd_ul_dl_config.h"
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
  external_ul_processor_example_impl(unsigned                               nof_rb,
                                     unsigned                               nof_ports_,
                                     std::optional<tdd_ul_dl_config_common> tdd_pattern,
                                     const std::string&                     processor_arguments) :
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

        // Initialize the TDD configuration required by the quiet processing.
        if (tdd_pattern) {
          tdd_pattern1 =
              tdd_pattern_description{.periodicity_slots   = tdd_pattern->pattern1.dl_ul_tx_period_nof_slots,
                                      .flexible_slot_idx   = tdd_pattern->pattern1.nof_dl_slots,
                                      .first_ul_symbol_idx = nof_slot_symbols - tdd_pattern->pattern1.nof_ul_symbols};
          if (tdd_pattern->pattern2) {
            tdd_pattern2 = tdd_pattern_description{
                .periodicity_slots = tdd_pattern->pattern2->dl_ul_tx_period_nof_slots,
                .flexible_slot_idx =
                    tdd_pattern->pattern1.dl_ul_tx_period_nof_slots + tdd_pattern->pattern2->nof_dl_slots,
                .first_ul_symbol_idx = nof_slot_symbols - tdd_pattern->pattern2->nof_ul_symbols};
          }
        }
      }
    }

    logger.info("PHY tap plugin constructed: nof_rb={}, nof_ports={}, quiet_processing={}, tdd_pattern_configured={}",
                nof_rb,
                nof_ports,
                enable_quiet_processing,
                tdd_pattern.has_value());
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

private:
  /// Stores the relevant TDD pattern information.
  struct tdd_pattern_description {
    /// Periodicity of the TDD pattern in number of slots.
    unsigned periodicity_slots;
    /// Index of the flexible slot within the TDD pattern.
    unsigned flexible_slot_idx;
    /// Index of the first UL symbol within the flexible slot.
    unsigned first_ul_symbol_idx;
  };

  /// Buffer for the temporary storage of the resource grid data.
  std::vector<cf_t> temp_buffer;
  /// Buffer for the temporary storage of the PRACH data.
  static_vector<cf_t, prach_constants::LONG_SEQUENCE_LENGTH> temp_buffer_prach;
  /// Number of ports to process.
  unsigned nof_ports;
  /// TDD pattern 1 information, if configured.
  std::optional<tdd_pattern_description> tdd_pattern1;
  /// TDD pattern 2 information, if configured.
  std::optional<tdd_pattern_description> tdd_pattern2;
  /// Index of the last processed symbol.
  unsigned last_processed_symbol = 0;
  /// Enables or disables the processing of quiet UL symbols.
  bool enable_quiet_processing = false;
  /// Set to true after the first \c process call, used to log a one-time activity confirmation.
  bool ul_symbol_activity_logged = false;
  /// Set to true after the first \c process_quiet call, used to log a one-time activity confirmation.
  bool quiet_symbol_activity_logged = false;
  /// Set to true after the first \c process_prach call, used to log a one-time activity confirmation.
  bool prach_activity_logged = false;
  /// Logger object.
  ocudulog::basic_logger& logger;
};

} // namespace ocudu
