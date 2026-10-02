// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief CUDA PDSCH symbol-generation paths versus the host chain, for every PDSCH modulation order.
///
/// For Qm = 2, 4, 6, 8 and 10 (1024QAM, TS 38.211 Section 5.1.7) the same LDPC codeblocks and configuration are fed to
/// - the host chain: ldpc_rate_matcher (bit selection TS 38.212 Section 5.4.2.1 and bit interleaving Section 5.4.2.2),
///   pseudo_random_generator scrambling (TS 38.211 Section 7.3.1.1) and modulation_mapper (Section 5.1); and
/// - every CUDA entry point that has a modulation-order specialised kernel (lib/phy/cuda pdsch_fused.cu and
///   modulation.cu).
/// INT8 outputs must equal the host ci8 symbols and demap to the host scrambled bits; FP16/FP32 outputs must lie
/// within the format precision of the host symbols and hard-decide to the same constellation point; BF16 grid outputs
/// must equal the host BF16 conversion of the scaled ci8 symbols.

#include "ocudu/adt/bit_buffer.h"
#include "ocudu/adt/complex.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <map>
#include <modulation.h>
#include <pdsch_fused.h>
#include <random>
#include <rate_matching.h>
#include <sstream>

using namespace ocudu;

namespace {

struct test_case {
  unsigned qm;
  unsigned bg;          // LDPC base graph (1 or 2).
  unsigned z;           // Lifting size.
  unsigned nof_filler;  // Filler bits per codeblock.
  unsigned rv;          // Redundancy version.
  unsigned nof_cbs;     // Number of codeblocks.
  unsigned nof_short;   // Number of codeblocks with the short rate-matched length.
  unsigned symbols_cb;  // Modulated symbols in a short codeblock (long codeblocks carry one more).
  bool     limited_buf; // Limited-buffer rate matching (Nref < N).
};

std::ostream& operator<<(std::ostream& os, const test_case& tc)
{
  return os << "Qm" << tc.qm << "_BG" << tc.bg << "_Z" << tc.z << "_F" << tc.nof_filler << "_rv" << tc.rv << "_C"
            << tc.nof_cbs << "_short" << tc.nof_short << "_S" << tc.symbols_cb << (tc.limited_buf ? "_lbrm" : "");
}

modulation_scheme to_modulation(unsigned qm)
{
  switch (qm) {
    case 2:
      return modulation_scheme::QPSK;
    case 4:
      return modulation_scheme::QAM16;
    case 6:
      return modulation_scheme::QAM64;
    case 8:
      return modulation_scheme::QAM256;
    default:
      return modulation_scheme::QAM1024;
  }
}

/// Host LDPC encoder buffer double holding an arbitrary (not necessarily valid) codeblock.
class codeblock_buffer : public ldpc_encoder_buffer
{
public:
  explicit codeblock_buffer(std::vector<uint8_t> bits_) : bits(std::move(bits_)) {}
  unsigned get_codeblock_length() const override { return bits.size(); }
  void     write_codeblock(span<uint8_t> data, unsigned offset) const override
  {
    std::copy(bits.begin() + offset, bits.begin() + offset + data.size(), data.begin());
  }

private:
  std::vector<uint8_t> bits;
};

/// Packs bit \c value at position \c i of a byte stream (MSB first within each byte) viewed as 32-bit words.
void set_bit_msb_byte(std::vector<uint32_t>& words, unsigned i, unsigned value)
{
  if (value != 0) {
    reinterpret_cast<uint8_t*>(words.data())[i / 8] |= static_cast<uint8_t>(0x80U >> (i % 8));
  }
}

/// Packs bit \c value at position \c i, MSB first within each 32-bit word.
void set_bit_msb_word(std::vector<uint32_t>& words, unsigned i, unsigned value)
{
  if (value != 0) {
    words[i / 32] |= 1U << (31 - (i % 32));
  }
}

/// Packs bit \c value at position \c i, LSB first within each 32-bit word.
void set_bit_lsb_word(std::vector<uint32_t>& words, unsigned i, unsigned value)
{
  if (value != 0) {
    words[i / 32] |= 1U << (i % 32);
  }
}

float half_to_float(uint16_t h)
{
  uint32_t sign = (h >> 15) & 1U;
  uint32_t exp  = (h >> 10) & 0x1fU;
  uint32_t mant = h & 0x3ffU;
  float    value;
  if (exp == 0) {
    value = std::ldexp(static_cast<float>(mant), -24);
  } else if (exp == 31) {
    value = (mant == 0) ? INFINITY : NAN;
  } else {
    value = std::ldexp(static_cast<float>(mant | 0x400U), static_cast<int>(exp) - 25);
  }
  return sign ? -value : value;
}

template <typename T>
class device_buffer
{
public:
  explicit device_buffer(size_t count) : size(count)
  {
    EXPECT_EQ(cudaMalloc(&ptr, std::max<size_t>(count, 1) * sizeof(T)), cudaSuccess);
    EXPECT_EQ(cudaMemset(ptr, 0, std::max<size_t>(count, 1) * sizeof(T)), cudaSuccess);
  }
  explicit device_buffer(const std::vector<T>& host) : device_buffer(host.size())
  {
    EXPECT_EQ(cudaMemcpy(ptr, host.data(), host.size() * sizeof(T), cudaMemcpyHostToDevice), cudaSuccess);
  }
  ~device_buffer() { cudaFree(ptr); }
  device_buffer(const device_buffer&)            = delete;
  device_buffer& operator=(const device_buffer&) = delete;

  T*             get() { return ptr; }
  std::vector<T> download() const
  {
    std::vector<T> host(size);
    EXPECT_EQ(cudaDeviceSynchronize(), cudaSuccess);
    EXPECT_EQ(cudaMemcpy(host.data(), ptr, size * sizeof(T), cudaMemcpyDeviceToHost), cudaSuccess);
    return host;
  }

private:
  T*     ptr = nullptr;
  size_t size;
};

bool cuda_device_available()
{
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess) {
    (void)cudaGetLastError();
    return false;
  }
  return count > 0;
}

/// Host reference for one transport block.
struct host_reference {
  // Encoded codeblocks as the GPU fused kernels read them: 2Z punctured bits followed by the host codeblock.
  std::vector<uint32_t> d_encoded_words;
  // Rate-matched, not yet interleaved bits per codeblock (MSB first within each byte), rm_stride words per codeblock.
  std::vector<uint32_t> rate_matched_words;
  unsigned              rm_stride = 0;
  // Interleaved, unscrambled codeword (one bit per byte) and the scrambled codeword.
  std::vector<uint8_t> codeword;
  std::vector<uint8_t> scrambled;
  // Host symbols.
  std::vector<ci8_t> symbols_ci8;
  std::vector<cf_t>  symbols_cf;
  float              scaling = 0;
};

class PdschCudaModulationOrderTest : public ::testing::TestWithParam<test_case>
{
protected:
  static constexpr unsigned rnti   = 0x4601;
  static constexpr unsigned n_id   = 37;
  static constexpr unsigned q      = 0;
  static constexpr unsigned c_init = (rnti << 15U) + (q << 14U) + n_id;

  void SetUp() override
  {
    if (!cuda_device_available()) {
      GTEST_SKIP() << "No CUDA device available.";
    }

    const test_case& tc = GetParam();
    rate_matcher        = create_ldpc_rate_matcher_factory_sw()->create();
    prg                 = create_pseudo_random_generator_sw_factory()->create();
    modulator           = create_modulation_mapper_factory()->create();
    ASSERT_TRUE(rate_matcher && prg && modulator);

    kb     = (tc.bg == 1) ? 22 : 10;
    n_cols = (tc.bg == 1) ? 68 : 52;
    N      = (n_cols - 2) * tc.z;
    n_cb   = N;
    nref   = 0;
    if (tc.limited_buf) {
      // A Nref that binds below N and is not a multiple of Z, as tbs_lbrm produces.
      nref = (N * 5) / 8 + 3;
      n_cb = nref;
    }
    E_short = tc.symbols_cb * tc.qm;
    E_long  = (tc.symbols_cb + 1) * tc.qm;
    if (tc.nof_short == tc.nof_cbs) {
      E_long = E_short;
    }
    G              = tc.nof_short * E_short + (tc.nof_cbs - tc.nof_short) * E_long;
    total_symbols  = G / tc.qm;
    encoded_stride = (n_cols * tc.z + 31) / 32;

    // Host scrambling sequence, packed for the GPU kernels (MSB-first and LSB-first words).
    dynamic_bit_buffer seq(G);
    prg->init(c_init);
    prg->generate(seq);
    seq_msb_word.assign((G + 31) / 32 + 1, 0);
    seq_lsb_word.assign((G + 31) / 32 + 1, 0);
    for (unsigned i = 0; i != G; ++i) {
      set_bit_msb_word(seq_msb_word, i, seq.extract(i, 1));
      set_bit_lsb_word(seq_lsb_word, i, seq.extract(i, 1));
    }

    // Constellation demapping table built from the host modulation mapper itself.
    modulation_scheme  mod        = to_modulation(tc.qm);
    unsigned           nof_points = 1U << tc.qm;
    dynamic_bit_buffer all_points(nof_points * tc.qm);
    for (unsigned p = 0; p != nof_points; ++p) {
      for (unsigned b = 0; b != tc.qm; ++b) {
        all_points.insert((p >> (tc.qm - 1 - b)) & 1U, p * tc.qm + b, 1);
      }
    }
    std::vector<ci8_t> points(nof_points);
    modulator->modulate(span<ci8_t>(points), all_points, mod);
    for (unsigned p = 0; p != nof_points; ++p) {
      unsigned key = pack_ci8(points[p]);
      ASSERT_EQ(demap.count(key), 0U) << "Host constellation is not one-to-one.";
      demap[key] = p;
    }
  }

  static unsigned pack_ci8(ci8_t s) { return (static_cast<uint8_t>(s.real()) << 8U) | static_cast<uint8_t>(s.imag()); }

  unsigned cb_rm_length(unsigned i_cb) const { return (i_cb < GetParam().nof_short) ? E_short : E_long; }

  host_reference make_reference(unsigned seed)
  {
    const test_case& tc = GetParam();
    host_reference   ref;
    std::mt19937     rgen(seed);

    ref.d_encoded_words.assign(static_cast<size_t>(tc.nof_cbs) * encoded_stride, 0);
    unsigned max_e = std::max(E_short, E_long);
    ref.rm_stride  = (max_e + 31) / 32;
    ref.rate_matched_words.assign(static_cast<size_t>(tc.nof_cbs) * ref.rm_stride, 0);
    ref.codeword.reserve(G);

    codeblock_metadata cfg          = {};
    cfg.tb_common.base_graph        = (tc.bg == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
    cfg.tb_common.lifting_size      = static_cast<ldpc::lifting_size_t>(tc.z);
    cfg.tb_common.rv                = tc.rv;
    cfg.tb_common.mod               = to_modulation(tc.qm);
    cfg.tb_common.Nref              = nref;
    cfg.cb_specific.nof_filler_bits = tc.nof_filler;

    for (unsigned i_cb = 0; i_cb != tc.nof_cbs; ++i_cb) {
      // Arbitrary codeblock bits: rate matching does not depend on parity consistency.
      std::vector<uint8_t> cb_bits(N);
      for (auto& bit : cb_bits) {
        bit = rgen() & 1U;
      }
      std::vector<uint32_t> cb_words(encoded_stride, 0);
      for (unsigned i = 0; i != 2 * tc.z; ++i) {
        set_bit_msb_byte(cb_words, i, rgen() & 1U); // Punctured systematic bits are never transmitted.
      }
      for (unsigned i = 0; i != N; ++i) {
        set_bit_msb_byte(cb_words, 2 * tc.z + i, cb_bits[i]);
      }
      std::copy(cb_words.begin(), cb_words.end(), ref.d_encoded_words.begin() + i_cb * encoded_stride);

      unsigned           E = cb_rm_length(i_cb);
      dynamic_bit_buffer rm_out(E);
      cfg.cb_specific.rm_length = E;
      rate_matcher->rate_match(rm_out, codeblock_buffer(cb_bits), cfg);

      // Interleaved output e; the GPU rate-matched-input path consumes f with f(i*K + j) = e(j*Qm + i).
      unsigned K = E / tc.qm;
      for (unsigned k = 0; k != E; ++k) {
        uint8_t bit = rm_out.extract(k, 1);
        ref.codeword.push_back(bit);
        unsigned                        j    = k / tc.qm;
        unsigned                        i    = k % tc.qm;
        std::vector<uint32_t>::iterator base = ref.rate_matched_words.begin() + i_cb * ref.rm_stride;
        if (bit) {
          reinterpret_cast<uint8_t*>(&*base)[(i * K + j) / 8] |= static_cast<uint8_t>(0x80U >> ((i * K + j) % 8));
        }
      }
    }

    // Host scrambling.
    ref.scrambled.resize(G);
    prg->init(c_init);
    prg->apply_xor(ref.scrambled, ref.codeword);

    // Host modulation.
    dynamic_bit_buffer packed(G);
    for (unsigned i = 0; i != G; ++i) {
      packed.insert(ref.scrambled[i], i, 1);
    }
    ref.symbols_ci8.resize(total_symbols);
    ref.symbols_cf.resize(total_symbols);
    ref.scaling = modulator->modulate(span<ci8_t>(ref.symbols_ci8), packed, to_modulation(tc.qm));
    modulator->modulate(span<cf_t>(ref.symbols_cf), packed, to_modulation(tc.qm));
    return ref;
  }

  pdsch_fused_tx_config_t make_fused_config() const
  {
    const test_case&        tc  = GetParam();
    pdsch_fused_tx_config_t cfg = {};
    cfg.N_cb                    = static_cast<int>(n_cb);
    cfg.N_full                  = static_cast<int>(n_cols * tc.z);
    cfg.k0                      = rate_matcher_compute_k0(tc.bg, tc.z, tc.rv, n_cb);
    cfg.F                       = static_cast<int>(tc.nof_filler);
    cfg.Kd                      = static_cast<int>(kb * tc.z - 2 * tc.z - tc.nof_filler);
    cfg.puncture_offset         = static_cast<int>(2 * tc.z);
    cfg.encoded_stride          = static_cast<int>(encoded_stride);
    cfg.nof_short               = static_cast<int>(tc.nof_short);
    cfg.E_short                 = static_cast<int>(E_short);
    cfg.E_long                  = static_cast<int>(E_long);
    cfg.num_cbs                 = static_cast<int>(tc.nof_cbs);
    cfg.mod_order               = static_cast<int>(tc.qm);
    cfg.total_symbols           = static_cast<int>(total_symbols);
    return cfg;
  }

  /// INT8 GPU symbols must equal the host ci8 symbols and demap to the host scrambled bits.
  void check_int8(const std::vector<int8_t>& gpu, const host_reference& ref, const char* path, unsigned offset = 0)
  {
    const test_case& tc          = GetParam();
    unsigned         sym_errors  = 0;
    unsigned         bit_errors  = 0;
    unsigned         first_error = total_symbols;
    for (unsigned i = 0; i != total_symbols; ++i) {
      ci8_t gpu_sym(gpu[2 * (offset + i)], gpu[2 * (offset + i) + 1]);
      if (!(gpu_sym == ref.symbols_ci8[i])) {
        ++sym_errors;
        first_error = std::min(first_error, i);
      }
      auto it = demap.find(pack_ci8(gpu_sym));
      if (it == demap.end()) {
        bit_errors += tc.qm;
        continue;
      }
      for (unsigned b = 0; b != tc.qm; ++b) {
        bit_errors += (((it->second >> (tc.qm - 1 - b)) & 1U) != ref.scrambled[i * tc.qm + b]);
      }
    }
    EXPECT_EQ(sym_errors, 0U) << path << ": first mismatching symbol " << first_error;
    EXPECT_EQ(bit_errors, 0U) << path << ": bit-domain mismatches";
  }

  /// Hard decision of a normalised component to the odd PAM level of the host constellation.
  int hard_level(float value, float scaling) const
  {
    int max_level = (1 << (GetParam().qm / 2)) - 1;
    int level     = 2 * static_cast<int>(std::floor(value / scaling / 2.0F)) + 1;
    return std::clamp(level, -max_level, max_level);
  }

  /// Floating-point GPU symbols must be within \c tolerance of the host symbols and hard-decide identically.
  void check_float(const std::vector<cf_t>& gpu, const host_reference& ref, float tolerance, const char* path)
  {
    unsigned errors        = 0;
    unsigned hard_errors   = 0;
    float    max_abs_error = 0;
    for (unsigned i = 0; i != total_symbols; ++i) {
      cf_t  host    = ref.symbols_cf[i];
      float error   = std::max(std::abs(gpu[i].real() - host.real()), std::abs(gpu[i].imag() - host.imag()));
      max_abs_error = std::max(max_abs_error, error);
      errors += (error > tolerance);
      hard_errors += (hard_level(gpu[i].real(), ref.scaling) != ref.symbols_ci8[i].real()) ||
                     (hard_level(gpu[i].imag(), ref.scaling) != ref.symbols_ci8[i].imag());
    }
    EXPECT_EQ(errors, 0U) << path << ": max_abs_error=" << max_abs_error;
    EXPECT_EQ(hard_errors, 0U) << path << ": hard-decision mismatches";
  }

  std::unique_ptr<ldpc_rate_matcher>       rate_matcher;
  std::unique_ptr<pseudo_random_generator> prg;
  std::unique_ptr<modulation_mapper>       modulator;
  std::map<unsigned, unsigned>             demap;
  std::vector<uint32_t>                    seq_msb_word;
  std::vector<uint32_t>                    seq_lsb_word;

  unsigned kb = 0, n_cols = 0, N = 0, n_cb = 0, nref = 0;
  unsigned E_short = 0, E_long = 0, G = 0, total_symbols = 0, encoded_stride = 0;
};

// Rate matching + interleaving + scrambling + modulation from LDPC-encoded codeblocks, INT8 output: the precomputed
// scrambling sequence kernel (production tb_encoder path) and the on-the-fly LFSR kernel.
TEST_P(PdschCudaModulationOrderTest, FusedEncodedToInt8MatchesHost)
{
  host_reference          ref = make_reference(0x5eed + GetParam().qm);
  pdsch_fused_tx_config_t cfg = make_fused_config();

  device_buffer<uint32_t> d_encoded(ref.d_encoded_words);
  device_buffer<uint32_t> d_seq(seq_msb_word);

  {
    device_buffer<int8_t> d_symbols(2 * total_symbols);
    ASSERT_EQ(pdsch_fused_encode_to_symbols_int8_precomputed(d_encoded.get(), d_seq.get(), d_symbols.get(), &cfg, 0),
              0);
    check_int8(d_symbols.download(), ref, "pdsch_fused_encode_to_symbols_int8_precomputed");
  }
  {
    device_buffer<int8_t> d_symbols(2 * total_symbols);
    ASSERT_EQ(pdsch_fused_encode_to_symbols_int8(d_encoded.get(), c_init, d_symbols.get(), &cfg, 0), 0);
    check_int8(d_symbols.download(), ref, "pdsch_fused_encode_to_symbols_int8");
  }
}

// Batched variant: two transport blocks with different codeblocks and the same scrambling sequence.
TEST_P(PdschCudaModulationOrderTest, FusedBatchEncodedToInt8MatchesHost)
{
  host_reference ref0 = make_reference(0xba7c0 + GetParam().qm);
  host_reference ref1 = make_reference(0xba7c1 + GetParam().qm);

  std::vector<uint32_t> encoded = ref0.d_encoded_words;
  encoded.insert(encoded.end(), ref1.d_encoded_words.begin(), ref1.d_encoded_words.end());
  device_buffer<uint32_t> d_encoded(encoded);
  device_buffer<uint32_t> d_seq(seq_msb_word);
  device_buffer<int8_t>   d_symbols(4 * total_symbols);

  pdsch_fused_tx_config_t cfg = make_fused_config();
  ASSERT_EQ(
      pdsch_fused_encode_to_symbols_int8_precomputed_batch(d_encoded.get(), d_seq.get(), d_symbols.get(), &cfg, 2, 0),
      0);
  std::vector<int8_t> gpu = d_symbols.download();
  check_int8(gpu, ref0, "pdsch_fused_encode_to_symbols_int8_precomputed_batch[0]", 0);
  check_int8(gpu, ref1, "pdsch_fused_encode_to_symbols_int8_precomputed_batch[1]", total_symbols);
}

// Direct BF16 resource-grid output (one layer, two ports): equal to the host mapper's BF16 conversion of ci8 * weight.
TEST_P(PdschCudaModulationOrderTest, FusedEncodedToBf16GridMatchesHost)
{
  host_reference          ref = make_reference(0xbf16 + GetParam().qm);
  pdsch_fused_tx_config_t cfg = make_fused_config();

  constexpr unsigned nof_ports = 2;
  unsigned           grid_re   = total_symbols + 16;
  float              weight    = ref.scaling * 0.70710678F; // Modulation scaling and a two-port weight.
  // Reversed RE order with a guard offset, so the RE offset table is exercised.
  constexpr unsigned    re_guard = 8;
  std::vector<uint32_t> re_offsets(total_symbols);
  for (unsigned i = 0; i != total_symbols; ++i) {
    re_offsets[i] = re_guard + (total_symbols - 1 - i);
  }
  device_buffer<uint32_t> d_encoded(ref.d_encoded_words);
  device_buffer<uint32_t> d_seq(seq_msb_word);
  device_buffer<uint32_t> d_offsets(re_offsets);
  device_buffer<uint16_t> d_grid(2 * nof_ports * grid_re);
  ASSERT_EQ(pdsch_fused_encode_to_bf16_grid_precomputed(d_encoded.get(),
                                                        d_seq.get(),
                                                        d_offsets.get(),
                                                        d_grid.get(),
                                                        &cfg,
                                                        0,
                                                        static_cast<int>(total_symbols),
                                                        nof_ports,
                                                        1,
                                                        static_cast<int>(grid_re),
                                                        weight,
                                                        0),
            0);
  std::vector<uint16_t> grid   = d_grid.download();
  unsigned              errors = 0;
  for (unsigned port = 0; port != nof_ports; ++port) {
    for (unsigned i = 0; i != total_symbols; ++i) {
      cbf16_t  expected(static_cast<float>(ref.symbols_ci8[i].real()) * weight,
                       static_cast<float>(ref.symbols_ci8[i].imag()) * weight);
      unsigned idx = 2 * (port * grid_re + re_offsets[i]);
      errors += (grid[idx] != expected.real.value()) || (grid[idx + 1] != expected.imag.value());
    }
  }
  EXPECT_EQ(errors, 0U) << "pdsch_fused_encode_to_bf16_grid_precomputed";
}

// Interleaving + scrambling + modulation from rate-matched (not interleaved) codeblocks, INT8 output.
TEST_P(PdschCudaModulationOrderTest, FusedRateMatchedToInt8MatchesHost)
{
  const test_case&        tc  = GetParam();
  host_reference          ref = make_reference(0x7a7e + tc.qm);
  device_buffer<uint32_t> d_rm(ref.rate_matched_words);
  device_buffer<int8_t>   d_symbols(2 * total_symbols);
  ASSERT_EQ(pdsch_fused_from_rate_matched_to_symbols_int8(d_rm.get(),
                                                          c_init,
                                                          d_symbols.get(),
                                                          static_cast<int>(ref.rm_stride),
                                                          static_cast<int>(tc.nof_short),
                                                          static_cast<int>(E_short),
                                                          static_cast<int>(E_long),
                                                          static_cast<int>(tc.qm),
                                                          static_cast<int>(total_symbols),
                                                          0),
            0);
  check_int8(d_symbols.download(), ref, "pdsch_fused_from_rate_matched_to_symbols_int8");
}

// Scrambling + modulation of the interleaved codeword: FP16 (pdsch_fused) and the modulation.cu entry points.
TEST_P(PdschCudaModulationOrderTest, ScrambleModulateMatchesHost)
{
  const test_case& tc  = GetParam();
  host_reference   ref = make_reference(0x5c4a + tc.qm);

  std::vector<uint32_t> cw_lsb((G + 31) / 32 + 1, 0);
  std::vector<uint32_t> cw_msb_byte((G + 31) / 32 + 1, 0);
  std::vector<uint32_t> scr_lsb((G + 31) / 32 + 1, 0);
  for (unsigned i = 0; i != G; ++i) {
    set_bit_lsb_word(cw_lsb, i, ref.codeword[i]);
    set_bit_msb_byte(cw_msb_byte, i, ref.codeword[i]);
    set_bit_lsb_word(scr_lsb, i, ref.scrambled[i]);
  }
  device_buffer<uint32_t> d_cw_lsb(cw_lsb);
  device_buffer<uint32_t> d_cw_msb_byte(cw_msb_byte);
  device_buffer<uint32_t> d_scr_lsb(scr_lsb);
  device_buffer<uint32_t> d_seq_lsb(seq_lsb_word);
  device_buffer<uint32_t> d_seq_msb(seq_msb_word);

  modulator_handle_t handle = nullptr;
  ASSERT_EQ(modulator_create(&handle), 0);

  auto to_cf = [this](const std::vector<cuFloatComplex>& in) {
    std::vector<cf_t> out(total_symbols);
    for (unsigned i = 0; i != total_symbols; ++i) {
      out[i] = {in[i].x, in[i].y};
    }
    return out;
  };

  {
    device_buffer<uint32_t> d_half(total_symbols);
    ASSERT_EQ(pdsch_fused_scramble_modulate_half(
                  d_cw_lsb.get(), d_seq_lsb.get(), d_half.get(), static_cast<int>(G), static_cast<int>(tc.qm), 0),
              0);
    std::vector<uint32_t> half = d_half.download();
    std::vector<cf_t>     gpu(total_symbols);
    for (unsigned i = 0; i != total_symbols; ++i) {
      gpu[i] = {half_to_float(half[i] & 0xffffU), half_to_float(half[i] >> 16U)};
    }
    // FP16 keeps 11 significant bits: |error| <= 2^-11 * 1.19 for the largest 1024QAM level.
    check_float(gpu, ref, 1e-3F, "pdsch_fused_scramble_modulate_half");
  }
  {
    device_buffer<cuFloatComplex> d_symbols(total_symbols);
    ASSERT_EQ(
        modulator_modulate(handle, d_scr_lsb.get(), d_symbols.get(), static_cast<int>(G), static_cast<int>(tc.qm), 0),
        0);
    check_float(to_cf(d_symbols.download()), ref, 1e-5F, "modulator_modulate");
  }
  {
    device_buffer<int8_t> d_symbols(2 * total_symbols);
    ASSERT_EQ(modulator_modulate_int8(
                  handle, d_scr_lsb.get(), d_symbols.get(), static_cast<int>(G), static_cast<int>(tc.qm), 0),
              0);
    check_int8(d_symbols.download(), ref, "modulator_modulate_int8");
  }
  {
    device_buffer<cuFloatComplex> d_symbols(total_symbols);
    ASSERT_EQ(
        modulator_scramble_and_modulate(
            handle, d_cw_lsb.get(), d_seq_lsb.get(), d_symbols.get(), static_cast<int>(G), static_cast<int>(tc.qm), 0),
        0);
    check_float(to_cf(d_symbols.download()), ref, 1e-5F, "modulator_scramble_and_modulate");
  }
  {
    device_buffer<int8_t> d_symbols(2 * total_symbols);
    ASSERT_EQ(modulator_scramble_and_modulate_int8(handle,
                                                   d_cw_msb_byte.get(),
                                                   d_seq_msb.get(),
                                                   d_symbols.get(),
                                                   static_cast<int>(G),
                                                   static_cast<int>(tc.qm),
                                                   0),
              0);
    check_int8(d_symbols.download(), ref, "modulator_scramble_and_modulate_int8");
  }
  if (E_short == E_long) {
    // The batch kernel takes uniform codeblocks, each LSB-first packed with its own word stride.
    unsigned              words_per_cb = (E_short + 31) / 32 + 1;
    std::vector<uint32_t> cbs(static_cast<size_t>(tc.nof_cbs) * words_per_cb, 0);
    for (unsigned i = 0; i != G; ++i) {
      unsigned i_cb = i / E_short;
      if (ref.codeword[i]) {
        cbs[i_cb * words_per_cb + (i % E_short) / 32] |= 1U << ((i % E_short) % 32);
      }
    }
    device_buffer<uint32_t>       d_cbs(cbs);
    device_buffer<cuFloatComplex> d_symbols(total_symbols);
    ASSERT_EQ(modulator_scramble_and_modulate_batch(handle,
                                                    d_cbs.get(),
                                                    d_seq_lsb.get(),
                                                    d_symbols.get(),
                                                    static_cast<int>(E_short),
                                                    static_cast<int>(words_per_cb),
                                                    static_cast<int>(tc.nof_cbs),
                                                    static_cast<int>(tc.qm),
                                                    0),
              0);
    check_float(to_cf(d_symbols.download()), ref, 1e-5F, "modulator_scramble_and_modulate_batch");
  }

  modulator_destroy(handle);
}

std::vector<test_case> make_test_cases()
{
  std::vector<test_case> cases;
  for (unsigned qm : {2U, 4U, 6U, 8U, 10U}) {
    // Single codeblock, BG1 Z=384, no filler, every RV.
    for (unsigned rv : {0U, 1U, 2U, 3U}) {
      cases.push_back({qm, 1, 384, 0, rv, 1, 1, 2000, false});
    }
    // Multiple codeblocks with short and long rate-matched lengths, filler bits, BG1 and BG2.
    cases.push_back({qm, 1, 208, 88, 0, 3, 1, 1531, false});
    cases.push_back({qm, 2, 96, 40, 2, 4, 2, 517, false});
    // Repetition (E > N_cb) and limited-buffer rate matching.
    cases.push_back({qm, 2, 20, 12, 3, 2, 2, 1201, false});
    cases.push_back({qm, 1, 352, 64, 1, 3, 0, 3331, true});
    cases.push_back({qm, 1, 384, 0, 2, 2, 2, 4099, true});
  }
  return cases;
}

std::string test_name(const ::testing::TestParamInfo<test_case>& info)
{
  std::stringstream ss;
  ss << info.param;
  return ss.str();
}

INSTANTIATE_TEST_SUITE_P(AllModulationOrders,
                         PdschCudaModulationOrderTest,
                         ::testing::ValuesIn(make_test_cases()),
                         test_name);

} // namespace
