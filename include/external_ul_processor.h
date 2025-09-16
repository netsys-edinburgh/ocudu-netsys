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
  /// This method is where the actual processing of the UL symbols takes place. Any external DSP processing must be
  /// implemented by this method. The \e grid_reader provides access to the resource grid symbols of the current \e
  /// slot, up to the current \e symbol.
  ///
  /// \param[out] grid_writer Resource grid writer, used to write the processed symbols back into the resource grid.
  /// \param[in]  grid_reader Resource grid reader, containing the input symbols to be processed.
  /// \param[in]  slot        Current slot.
  /// \param[in]  symbol      Current symbol index within the slot.
  virtual void process(resource_grid_writer&       grid_writer,
                       const resource_grid_reader& grid_reader,
                       slot_point                  slot,
                       unsigned                    symbol) = 0;
};

} // namespace srsran
