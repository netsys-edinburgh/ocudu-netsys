// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/phy/lower/modulation/phase_compensation_lut.h"
#include "low_phy_tx.h"
#include "pdxch_baseband_modulator_accelerator.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/resource_block.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace ocudu;

namespace {

static_assert(sizeof(cbf16_t) == sizeof(uint32_t), "CUDA low-PHY TX expects packed complex BF16 resource elements.");
static_assert(pdxch_cuda_max_nof_beams_per_slot == OCUDU_LOWPHY_TX_MAX_BEAMS,
              "The documented beam limit must match the CUDA low-PHY TX one.");

std::atomic<bool> logged_lowphy_tx_device_grid_path{false};
std::atomic<bool> logged_lowphy_tx_host_grid_path{false};
std::atomic<bool> logged_lowphy_tx_unavailable_path{false};

static bool env_flag_disabled(const char* name)
{
  const char* value = std::getenv(name);
  return value && ((std::strcmp(value, "0") == 0) || (std::strcmp(value, "false") == 0) ||
                   (std::strcmp(value, "off") == 0) || (std::strcmp(value, "no") == 0));
}

static float dB_to_amplitude(float value)
{
  return std::pow(10.0F, value / 20.0F);
}

class pdxch_baseband_modulator_cuda : public pdxch_baseband_modulator_accelerator
{
public:
  pdxch_baseband_modulator_cuda(subcarrier_spacing scs_,
                                cyclic_prefix      cp_,
                                sampling_rate      srate_,
                                unsigned           bandwidth_rb_,
                                double             center_freq_Hz_,
                                unsigned           nof_ports_,
                                float              modulator_gain_dB_) :
    scs(scs_),
    cp(cp_),
    srate(srate_),
    bandwidth_rb(bandwidth_rb_),
    center_freq_Hz(center_freq_Hz_),
    nof_ports(nof_ports_),
    nof_symbols_per_slot(get_nsymb_per_slot(cp)),
    dft_size(srate.get_dft_size(scs)),
    rg_size(bandwidth_rb * NOF_SUBCARRIERS_PER_RB),
    modulator_gain_dB(modulator_gain_dB_),
    phase_table(scs, cp, dft_size, center_freq_Hz, true),
    next_center_freq_Hz(center_freq_Hz)
  {
    use_host_grid_staging = !env_flag_disabled("OCUDU_LOWPHY_TX_HOST_GRID_STAGING");
    warmup_handle();
  }

  ~pdxch_baseband_modulator_cuda() override
  {
    if (handle != nullptr) {
      ocudu_lowphy_tx_destroy(handle);
      handle = nullptr;
    }
  }

  bool enqueue(baseband_gateway_buffer_dynamic& output,
               const resource_grid_reader&      grid,
               unsigned                         i_symbol_sf_begin,
               span<const unsigned>             symbol_sizes_sf) override
  {
    if ((nof_ports != output.get_nof_channels()) || (symbol_sizes_sf.size() != nof_symbols_per_slot)) {
      return false;
    }

    // Without a supported beam selection the slot is left to the software modulator.
    std::optional<beam_selection> beams = select_beams(grid);
    if (!beams.has_value()) {
      return false;
    }

    update_phase_table_if_needed();

    ocudu_lowphy_tx_config_t cfg = make_config(i_symbol_sf_begin, symbol_sizes_sf, *beams);

    if (!ensure_handle(cfg)) {
      return false;
    }

    std::array<void*, OCUDU_LOWPHY_TX_MAX_PORTS> output_ports = {};
    for (unsigned port = 0; port != nof_ports; ++port) {
      span<ci16_t> port_buffer = output[port];
      if (port_buffer.size() != static_cast<size_t>(cfg.nof_samples)) {
        return false;
      }
      output_ports[port] = port_buffer.data();
    }

    void* stream = ocudu_lowphy_tx_get_stream(handle);
    if (grid.supports_device_grid_reading() && grid.prepare_device_grid_reading(stream)) {
      if (!logged_lowphy_tx_device_grid_path.exchange(true)) {
        ocudulog::fetch_basic_logger("PHY").info(
            "Lower-PHY TX GPU path selected: direct CUDA-visible downlink resource-grid reader.");
      }
      bool enqueued = ocudu_lowphy_tx_process(handle, grid.get_device_grid_cbf16(), output_ports.data(), stream) != 0;
      if (enqueued) {
        if (!grid.on_device_grid_reading_enqueued(stream)) {
          (void)ocudu_lowphy_tx_synchronize(handle);
        }
      } else {
        grid.release_device_grid_reading();
      }
      return enqueued;
    }

    if (!use_host_grid_staging) {
      if (!logged_lowphy_tx_unavailable_path.exchange(true)) {
        ocudulog::fetch_basic_logger("PHY").warning(
            "Lower-PHY TX GPU path unavailable: resource grid is not CUDA-visible and host-grid staging is disabled.");
      }
      return false;
    }

    std::array<const void*, OCUDU_LOWPHY_TX_MAX_BEAMS> beam_grids = {};
    for (unsigned i_beam = 0, i_beam_end = beams->grid_ports.size(); i_beam != i_beam_end; ++i_beam) {
      beam_grids[i_beam] = get_contiguous_host_port(grid, beams->grid_ports[i_beam]);
      if (beam_grids[i_beam] == nullptr) {
        if (!logged_lowphy_tx_unavailable_path.exchange(true)) {
          ocudulog::fetch_basic_logger("PHY").warning(
              "Lower-PHY TX GPU path unavailable: downlink resource grid is not contiguous for host-grid staging.");
        }
        return false;
      }
    }
    if (!logged_lowphy_tx_host_grid_path.exchange(true)) {
      ocudulog::fetch_basic_logger("PHY").info("Lower-PHY TX GPU path selected: host resource-grid staging fallback.");
    }
    return ocudu_lowphy_tx_process_host_beams(handle, beam_grids.data(), output_ports.data(), stream) != 0;
  }

  bool wait() override { return (handle != nullptr) && (ocudu_lowphy_tx_synchronize(handle) != 0); }

  bool prepare_output_buffer(baseband_gateway_buffer_dynamic& output) override
  {
    if ((handle == nullptr) || (output.get_nof_channels() != nof_ports)) {
      return false;
    }

    span<ci16_t> first_port = output[0];
    if (first_port.data() == nullptr || first_port.empty()) {
      return false;
    }

    for (unsigned port = 1; port != nof_ports; ++port) {
      span<ci16_t> port_samples = output[port];
      if (port_samples.data() != first_port.data() + static_cast<size_t>(port) * first_port.size()) {
        return false;
      }
    }

    size_t total_bytes = static_cast<size_t>(first_port.size()) * nof_ports * sizeof(ci16_t);
    return ocudu_lowphy_tx_register_host_output(handle, first_port.data(), total_bytes) != 0;
  }

  void set_center_frequency(double center_freq_Hz_) override
  {
    next_center_freq_Hz.store(center_freq_Hz_, std::memory_order_relaxed);
  }

  bool set_beam_weights(const beam_weights_codebook& codebook) override
  {
    if ((codebook.get_nof_antennas() != nof_ports) || (nof_ports > OCUDU_LOWPHY_TX_MAX_PORTS) ||
        (codebook.get_nof_beams() == 0)) {
      return false;
    }

    nof_codebook_beams = codebook.get_nof_beams();
    codebook_weights.resize(static_cast<size_t>(nof_ports) * nof_codebook_beams);
    for (unsigned i_antenna = 0; i_antenna != nof_ports; ++i_antenna) {
      span<const cf_t> antenna_weights = codebook.get_antenna_coefficients(i_antenna);
      std::copy(
          antenna_weights.begin(), antenna_weights.end(), codebook_weights.begin() + i_antenna * nof_codebook_beams);
    }

    // Beams that do not reach any antenna port never need to be read.
    beam_is_used.assign(nof_codebook_beams, false);
    for (unsigned i_beam = 0; i_beam != nof_codebook_beams; ++i_beam) {
      for (unsigned i_antenna = 0; i_antenna != nof_ports; ++i_antenna) {
        beam_is_used[i_beam] = beam_is_used[i_beam] || (get_weight(i_antenna, i_beam) != cf_t());
      }
    }

    // The identity codebook maps grid port i to antenna port i, which the kernel does without weighting.
    direct_mapping = (nof_codebook_beams == nof_ports);
    for (unsigned i_antenna = 0; direct_mapping && (i_antenna != nof_ports); ++i_antenna) {
      for (unsigned i_beam = 0; i_beam != nof_codebook_beams; ++i_beam) {
        direct_mapping = direct_mapping && (get_weight(i_antenna, i_beam) == cf_t((i_beam == i_antenna) ? 1.0F : 0.0F));
      }
    }
    return true;
  }

private:
  /// Resource grid ports, i.e. beams, that the GPU reads for a slot.
  struct beam_selection {
    /// True if grid port i feeds antenna port i without weighting.
    bool direct = true;
    /// Grid ports read for the slot, in beam order.
    static_vector<unsigned, OCUDU_LOWPHY_TX_MAX_BEAMS> grid_ports;
  };

  /// Gets the codebook weight of a beam on an antenna port.
  cf_t get_weight(unsigned i_antenna, unsigned i_beam) const
  {
    return codebook_weights[i_antenna * nof_codebook_beams + i_beam];
  }

  /// \brief Selects the beams the GPU reads for the slot held by \c grid.
  ///
  /// A codebook with no more beams than the GPU combines is mapped whole, so the configuration does not change from
  /// slot to slot. A larger one is reduced to the beams that carry data in the slot and reach an antenna port.
  ///
  /// \return The selected beams, or nothing if the slot uses more beams than the GPU combines.
  std::optional<beam_selection> select_beams(const resource_grid_reader& grid) const
  {
    beam_selection selection;
    if (codebook_weights.empty() || direct_mapping) {
      for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
        selection.grid_ports.push_back(i_port);
      }
      return selection;
    }

    selection.direct = false;
    if (nof_codebook_beams <= OCUDU_LOWPHY_TX_MAX_BEAMS) {
      for (unsigned i_beam = 0; i_beam != nof_codebook_beams; ++i_beam) {
        selection.grid_ports.push_back(i_beam);
      }
      return selection;
    }

    for (unsigned i_beam = 0; i_beam != nof_codebook_beams; ++i_beam) {
      if (!beam_is_used[i_beam] || !has_allocation(grid, i_beam)) {
        continue;
      }
      if (selection.grid_ports.full()) {
        return std::nullopt;
      }
      selection.grid_ports.push_back(i_beam);
    }
    return selection;
  }

  /// Determines whether any symbol of the slot carries data in the given grid port.
  bool has_allocation(const resource_grid_reader& grid, unsigned port) const
  {
    for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol) {
      if (!grid.get_allocation_range(port, i_symbol).empty()) {
        return true;
      }
    }
    return false;
  }

  ocudu_lowphy_tx_config_t
  make_config(unsigned i_symbol_sf_begin, span<const unsigned> symbol_sizes_sf, const beam_selection& beams)
  {
    ocudu_lowphy_tx_config_t cfg = {};
    cfg.dft_size                 = dft_size;
    cfg.rg_size                  = rg_size;
    cfg.nof_ports                = nof_ports;
    cfg.nof_symbols              = nof_symbols_per_slot;
    // Same scaling as the software OFDM modulator: the modulator gain normalizes the signal and applies the back-off,
    // and the conversion to 16-bit samples saturates, without a separate clipping stage.
    cfg.ofdm_scale       = 1.0F;
    cfg.amplitude_gain   = dB_to_amplitude(modulator_gain_dB);
    cfg.clipping_enabled = 0;
    cfg.clipping_ceiling = 0.0F;
    if (!beams.direct) {
      unsigned nof_beams = beams.grid_ports.size();
      cfg.nof_beams      = nof_beams;
      for (unsigned i_beam = 0; i_beam != nof_beams; ++i_beam) {
        unsigned grid_port          = beams.grid_ports[i_beam];
        cfg.beam_grid_ports[i_beam] = grid_port;
        for (unsigned i_antenna = 0; i_antenna != nof_ports; ++i_antenna) {
          cf_t weight                                         = get_weight(i_antenna, grid_port);
          cfg.beam_weights_re[i_antenna * nof_beams + i_beam] = weight.real();
          cfg.beam_weights_im[i_antenna * nof_beams + i_beam] = weight.imag();
        }
      }
    }

    unsigned sample_offset = 0;
    for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol) {
      unsigned i_symbol_sf         = i_symbol_sf_begin + i_symbol;
      unsigned cp_len              = cp.get_length(i_symbol_sf, scs).to_samples(srate.to_Hz());
      cfg.cp_lengths[i_symbol]     = cp_len;
      cfg.symbol_offsets[i_symbol] = sample_offset;
      cf_t phase                   = phase_table.get_coefficient(i_symbol_sf);
      cfg.phase_re[i_symbol]       = phase.real();
      cfg.phase_im[i_symbol]       = phase.imag();
      sample_offset += symbol_sizes_sf[i_symbol];
    }
    cfg.nof_samples = sample_offset;
    return cfg;
  }

  void warmup_handle()
  {
    std::array<unsigned, MAX_NSYMB_PER_SLOT> initial_symbol_sizes = {};
    for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol) {
      unsigned cp_len                = cp.get_length(i_symbol, scs).to_samples(srate.to_Hz());
      initial_symbol_sizes[i_symbol] = dft_size + cp_len;
    }
    span<const unsigned>     symbol_sizes(initial_symbol_sizes.data(), nof_symbols_per_slot);
    // Until the codebook is set, the grid ports map onto the antenna ports.
    ocudu_lowphy_tx_config_t cfg = make_config(0, symbol_sizes, beam_selection{});
    if (handle == nullptr) {
      (void)ocudu_lowphy_tx_create(&cfg, &handle);
    }
  }

  void update_phase_table_if_needed()
  {
    double next_freq_Hz = next_center_freq_Hz.load(std::memory_order_relaxed);
    if (next_freq_Hz != center_freq_Hz) {
      phase_table    = phase_compensation_lut(scs, cp, dft_size, next_freq_Hz, true);
      center_freq_Hz = next_freq_Hz;
    }
  }

  bool ensure_handle(const ocudu_lowphy_tx_config_t& cfg)
  {
    if (handle == nullptr) {
      return ocudu_lowphy_tx_create(&cfg, &handle) != 0;
    }
    return ocudu_lowphy_tx_update_config(handle, &cfg) != 0;
  }

  /// Gets the symbols of a grid port if they are contiguous in host memory, otherwise \c nullptr.
  const void* get_contiguous_host_port(const resource_grid_reader& grid, unsigned port) const
  {
    span<const cbf16_t> base = grid.get_view(port, 0);
    if ((base.data() == nullptr) || (base.size() != rg_size)) {
      return nullptr;
    }

    for (unsigned symbol = 1; symbol != nof_symbols_per_slot; ++symbol) {
      span<const cbf16_t> view = grid.get_view(port, symbol);
      if ((view.data() != base.data() + static_cast<size_t>(symbol) * rg_size) || (view.size() != rg_size)) {
        return nullptr;
      }
    }
    return base.data();
  }

  subcarrier_spacing scs;
  cyclic_prefix      cp;
  sampling_rate      srate;
  unsigned           bandwidth_rb;
  double             center_freq_Hz;
  unsigned           nof_ports;
  /// Number of beams in the codebook, i.e. resource grid ports.
  unsigned nof_codebook_beams = 0;
  /// Codebook weights, antenna major. Empty until the codebook is set, which maps grid port i to antenna port i.
  std::vector<cf_t> codebook_weights;
  /// Flags, for each beam, whether the codebook maps it onto any antenna port.
  std::vector<bool> beam_is_used;
  /// True if the codebook is the identity.
  bool                      direct_mapping = true;
  unsigned                  nof_symbols_per_slot;
  unsigned                  dft_size;
  unsigned                  rg_size;
  float                     modulator_gain_dB;
  phase_compensation_lut    phase_table;
  std::atomic<double>       next_center_freq_Hz;
  ocudu_lowphy_tx_handle_t* handle                = nullptr;
  bool                      use_host_grid_staging = true;
};

} // namespace

std::unique_ptr<pdxch_baseband_modulator_accelerator>
ocudu::create_pdxch_baseband_modulator_accelerator_cuda(subcarrier_spacing scs,
                                                        cyclic_prefix      cp,
                                                        sampling_rate      srate,
                                                        unsigned           bandwidth_rb,
                                                        double             center_freq_Hz,
                                                        unsigned           nof_ports,
                                                        float              modulator_gain_dB)
{
  return std::make_unique<pdxch_baseband_modulator_cuda>(
      scs, cp, srate, bandwidth_rb, center_freq_Hz, nof_ports, modulator_gain_dB);
}
