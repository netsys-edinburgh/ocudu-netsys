// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/tensor.h"
#include "ocudu/phy/upper/rx_buffer_decoder_callback.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include <utility>
#include <vector>

namespace ocudu {

class rx_buffer_spy : public unique_rx_buffer::buffer_management
{
public:
  rx_buffer_spy() = default;

  rx_buffer_spy(unsigned max_codeblock_size, unsigned nof_codeblocks) :
    soft_bits({max_codeblock_size, nof_codeblocks}), crc(nof_codeblocks)
  {
    for (unsigned i_cb = 0; i_cb != nof_codeblocks; ++i_cb) {
      hard_bits.emplace_back(dynamic_bit_buffer(max_codeblock_size));
    }
  }

  unsigned get_nof_codeblocks() const override
  {
    ++const_count;
    return soft_bits.get_dimensions_size().back();
  }
  void       reset_codeblocks_crc() override { ++count; }
  span<bool> get_codeblocks_crc() override
  {
    ++count;
    return span<bool>(reinterpret_cast<bool*>(crc.data()), crc.size());
  }
  unsigned get_absolute_codeblock_id(unsigned codeblock_id) const override
  {
    ++const_count;
    return 0;
  }
  span<log_likelihood_ratio> get_codeblock_soft_bits(unsigned codeblock_id, unsigned data_size) override
  {
    ++count;
    return soft_bits.get_view<1>({codeblock_id}).first(data_size);
  }
  bit_buffer get_codeblock_data_bits(unsigned codeblock_id, unsigned data_size) override
  {
    ++count;
    return hard_bits[codeblock_id].first(data_size);
  }
  void decode_cb_in_sequence(unsigned                    retransmission,
                             unsigned                    codeblock_id,
                             rx_buffer_decoder_callback& decoder_callback) override
  {
    if (defer_decode) {
      pending_decodes.emplace_back(codeblock_id, &decoder_callback);
      return;
    }
    decoder_callback.codeblock_decode(codeblock_id);
  }

  /// \brief Sets whether the codeblock decodes wait for \ref clear_pending_decode, as they do while an earlier
  /// repetition of a TTI bundle is being decoded.
  void set_defer_decode(bool defer) { defer_decode = defer; }

  /// Gets the number of codeblock decodes waiting to be cleared.
  unsigned get_nof_pending_decodes() const { return pending_decodes.size(); }

  /// Clears the oldest codeblock decode waiting.
  void clear_pending_decode()
  {
    auto [codeblock_id, callback] = pending_decodes.front();
    pending_decodes.erase(pending_decodes.begin());
    callback->codeblock_decode(codeblock_id);
  }

  bool try_lock()
  {
    locked = true;
    return true;
  }

  void unlock(unsigned) override
  {
    locked = false;
    ++nof_unlocks;
  }

  void release(unsigned) override
  {
    locked = false;
    ++nof_releases;
  }

  /// Gets the number of times the buffer was unlocked without releasing it.
  unsigned get_nof_unlocks() const { return nof_unlocks; }

  /// Gets the number of times the buffer was released.
  unsigned get_nof_releases() const { return nof_releases; }

  /// Clears all counters.
  void clear()
  {
    count       = 0;
    const_count = 0;
  }

  /// Get the total number of times the methods have been called.
  unsigned get_total_count() const { return count + const_count; }

  /// Returns true if the buffer is locked.
  bool is_locked() const { return locked; }

private:
  unsigned         count       = 0;
  mutable unsigned const_count = 0;
  bool             locked      = false;
  unsigned         nof_unlocks  = 0;
  unsigned         nof_releases = 0;
  bool             defer_decode = false;

  std::vector<std::pair<unsigned, rx_buffer_decoder_callback*>> pending_decodes;

  dynamic_tensor<2, log_likelihood_ratio> soft_bits;
  std::vector<dynamic_bit_buffer>         hard_bits;
  std::vector<uint8_t>                    crc;
};

} // namespace ocudu
