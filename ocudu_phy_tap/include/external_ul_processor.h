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

#include "srsran/phy/upper/upper_phy_rx_symbol_handler.h"

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
  /// called from this method.
  ///
  /// \param[in] context The context of the received UL symbol, including sector, slot, and symbol index.
  /// \param[in] grid    The resource grid containing the UL symbols to process.
  virtual void process(const upper_phy_rx_symbol_context& context, const shared_resource_grid& grid) = 0;
};

} // namespace srsran
