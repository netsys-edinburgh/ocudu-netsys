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
#include "srsran/phy/upper/uplink_pdu_slot_repository.h"
#include "srsran/ran/slot_point.h"

namespace srsran {

/// \brief Interface for processing uplink symbols present in the resource grid.
///
/// This interface allows for external processing of uplink symbols received in the resource grid.
class external_ul_processor
{
public:
  /// Default destructor.
  virtual ~external_ul_processor() = default;

  /// \brief Processes the UL symbols in the resource grid.
  ///
  /// This method is where the actual processing of the UL symbols takes place. It is called on a symbol basis and will
  /// process the indicated symbol. Any external DSP processing must be implemented by this method.
  ///
  /// \param[out] grid_writer   Resource grid writer, used to write the processed symbols back into the resource grid.
  /// \param[in]  grid_reader   Resource grid reader, containing the input symbols to be processed.
  /// \param[in]  slot          Current slot.
  /// \param[in]  symbol        Current symbol index within the slot.
  /// \param[in]  pusch_pdus    PUSCH PDUs scheduled in the slot up to the current symbol.
  /// \param[in]  pucch_pdus    PUCCH PDUs scheduled in the slot up to the current symbol.
  /// \param[in]  pucch_f1_pdus Common parameters of PUCCH Format 1 PDUs scheduled up to the current symbol.
  /// \param[in]  srs_pdus      SRS PDUs scheduled in the slot up to the current symbol.
  virtual void process(resource_grid_writer&                                     grid_writer,
                       const resource_grid_reader&                               grid_reader,
                       slot_point                                                slot,
                       unsigned                                                  symbol,
                       span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                       span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                       span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                       span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) = 0;

  /// \brief Processes the non-allocated UL symbols in the resource grid.
  ///
  /// This method is where the actual processing of non-allocated UL symbols takes place and is called on a slot basis
  /// and will process all unallocated symbols in the indicated slot. Any external DSP processing must be implemented by
  /// this method.
  ///
  /// \param[in]  grid_reader   Resource grid reader, containing the input symbols to be processed.
  /// \param[in]  slot          Current slot.
  virtual void process_quiet(const resource_grid_reader& grid_reader, slot_point slot) = 0;
};

} // namespace srsran
