// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "processor_pool_helpers.h"
#include "ocudu/adt/circular_array.h"
#include "ocudu/phy/upper/uplink_pdu_slot_repository.h"
#include "ocudu/phy/upper/uplink_processor.h"
#include "ocudu/phy/upper/uplink_slot_processor.h"

namespace ocudu {

/// Defines the structure to configure the uplink processor pool.
struct uplink_processor_pool_impl_config {
  /// Set of uplink processors for a numerology.
  struct uplink_processor_set {
    /// Subcarrier spacing.
    subcarrier_spacing scs;
    /// Vector of uplink processors for this numerology.
    std::vector<std::unique_ptr<uplink_processor>> procs;
  };

  /// Vector of \c info objects, which contains the uplink processors for a given numerology.
  std::vector<uplink_processor_set> procs;
  /// Default uplink processor used for handling PRACH and receive symbols when no processor has been assigned.
  std::unique_ptr<uplink_processor> default_processor;
};

/// Uplink processor pool implementation.
class uplink_processor_pool_impl : public uplink_processor_pool,
                                   private uplink_slot_processor_pool,
                                   private uplink_pdu_slot_repository_pool
{
public:
  /// \brief Constructs an uplink processor pool with the given configuration.
  explicit uplink_processor_pool_impl(uplink_processor_pool_impl_config dl_processors);

  // See uplink_processor_pool interface for documentation.
  uplink_slot_processor_pool& get_slot_processor_pool() override { return *this; }

  // See uplink_processor_pool interface for documentation.
  uplink_pdu_slot_repository_pool& get_slot_pdu_repository_pool() override { return *this; }

  // See uplink_pdu_slot_repository_pool interface for documentation.
  void stop() override;

private:
  /// \brief Number of slots the processor assignation table spans.
  ///
  /// The table is indexed by slot, so it must span every slot from the newest UL_TTI.request to the oldest slot whose
  /// receive symbols are still arriving: the maximum processing delay, plus the lead of the slot indications over the
  /// air time (the transmit lead of a split-8 radio), plus the receive latency. When it spans less, a request for a
  /// later slot overwrites the entry of a slot still being received, that slot's symbols reach a processor configured
  /// for another slot, and its PDUs are discarded when the processor is reused. Sixteen entries are only 8 ms at 30 kHz,
  /// which a GPU deployment with a processing delay of 8 slots exceeds. The size divides the number of slots in an SFN
  /// period at every numerology, so the index stays continuous across the SFN wrap.
  static constexpr unsigned nof_assigned_processors = 64;

  // See uplink_slot_processor_pool interface for documentation.
  uplink_slot_processor& get_slot_processor(slot_point slot) override;

  // See uplink_pdu_slot_repository_pool interface for documentation.
  unique_uplink_pdu_slot_repository get_pdu_slot_repository(slot_point slot) override;

  /// Repository of uplink processors.
  processor_pool_repository<uplink_processor> processors;
  /// Default uplink processor used for handling PRACH and receive symbols when no processor has been assigned.
  std::unique_ptr<uplink_processor> default_processor;
  /// Circular uplink processor assignation.
  circular_array<std::atomic<uplink_processor*>, nof_assigned_processors> assigned_processors = {};
};

} // namespace ocudu
