// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/transmitter/ofh_data_flow_uplane_downlink_data_impl.h"
#include "../../phy/support/resource_grid_test_doubles.h"
#include "../compression/ofh_iq_compressor_test_doubles.h"
#include "../ecpri/ecpri_packet_builder_test_doubles.h"
#include "../ethernet/vlan_ethernet_frame_builder_test_doubles.h"
#include "ocudu/adt/format.h"
#include "ocudu/adt/interval.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include "ocudu/ofh/ethernet/ethernet_frame_pool.h"
#include "ocudu/phy/support/resource_grid_context.h"
#include "ocudu/ran/resource_block.h"
#include <array>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using namespace ocudu;
using namespace ofh;

namespace {

/// Spy OFH User-Plane packet builder.
class ofh_uplane_packet_builder_spy : public uplane_message_builder
{
  static_vector<uplane_message_params, MAX_NSYMB_PER_SLOT> uplane_msg_params;
  std::vector<std::vector<cbf16_t>>                        iq_data;

public:
  ofh_uplane_packet_builder_spy()
  {
    iq_data.resize(MAX_NSYMB_PER_SLOT);
    for (auto& iq_symbol : iq_data) {
      iq_symbol.resize(MAX_NOF_SUBCARRIERS);
    }
  }

  units::bytes get_header_size(const ru_compression_params& params) const override { return units::bytes(8); }

  unsigned build_message(span<uint8_t> buffer, span<const cbf16_t> grid, const uplane_message_params& params) override
  {
    std::copy(grid.begin(), grid.end(), iq_data[params.symbol_id].begin() + params.start_prb * NOF_SUBCARRIERS_PER_RB);
    uplane_msg_params.push_back(params);

    return 0;
  }

  /// Returns the number of built packets.
  unsigned nof_built_packets() const { return uplane_msg_params.size(); }

  /// Retuns a span of the User-Plane message parameters processed by this builder.
  span<const uplane_message_params> get_uplane_params() const { return uplane_msg_params; }

  /// Returns a pointer to the resource grid reader processed by this builder.
  span<const cbf16_t> get_iq_data(unsigned symbol) const { return iq_data[symbol]; }
};

} // namespace

class ofh_data_flow_uplane_downlink_data_impl_fixture : public ::testing::TestWithParam<ru_compression_params>
{
protected:
  const unsigned                          nof_symbols;
  const unsigned                          ru_nof_prbs;
  const unsigned                          du_nof_prbs;
  const ether::vlan_frame_params          vlan_params  = {{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x11},
                                                          {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x22},
                                                          ether::vlan_parameters{.tci_vid = 1, .tci_pcp = 7},
                                                          0xaabb};
  const ru_compression_params             compr_params = GetParam();
  data_flow_uplane_downlink_data_impl     data_flow;
  ether::testing::vlan_frame_builder_spy* vlan_builder;
  ecpri::testing::packet_builder_spy*     ecpri_builder;
  ofh_uplane_packet_builder_spy*          uplane_builder;
  resource_grid_reader_spy                rg_reader_spy;
  resource_grid_writer_spy                rg_writer_spy;
  resource_grid_spy                       rg_spy;
  shared_resource_grid_spy                shared_rg_spy;

  ofh_data_flow_uplane_downlink_data_impl_fixture() :
    nof_symbols(3),
    ru_nof_prbs(273),
    du_nof_prbs(273),
    data_flow(get_config(), generate_data_flow_dependencies()),
    rg_reader_spy(1, nof_symbols, du_nof_prbs),
    rg_spy(rg_reader_spy, rg_writer_spy),
    shared_rg_spy(rg_spy)
  {
    initialize_grid_reader();
  }

  data_flow_uplane_downlink_data_impl_config get_config()
  {
    data_flow_uplane_downlink_data_impl_config config;
    config.ru_nof_prbs  = ru_nof_prbs;
    config.compr_params = compr_params;

    return config;
  }

  data_flow_uplane_downlink_data_impl_dependencies generate_data_flow_dependencies()
  {
    data_flow_uplane_downlink_data_impl_dependencies dependencies;
    dependencies.logger         = &ocudulog::fetch_basic_logger("TEST");
    dependencies.compressor_sel = std::make_unique<ofh::testing::iq_compressor_dummy>();
    dependencies.frame_pool     = std::make_shared<ether::eth_frame_pool>(
        *dependencies.logger, units::bytes(9000), 2, ofh::message_type::user_plane, ofh::data_direction::downlink);

    {
      auto temp               = std::make_unique<ofh_uplane_packet_builder_spy>();
      uplane_builder          = temp.get();
      dependencies.up_builder = std::move(temp);
    }
    {
      auto temp                = std::make_unique<ether::testing::vlan_frame_builder_spy>(vlan_params);
      vlan_builder             = temp.get();
      dependencies.eth_builder = std::move(temp);
    }
    {
      auto temp                  = std::make_unique<ecpri::testing::packet_builder_spy>();
      ecpri_builder              = temp.get();
      dependencies.ecpri_builder = std::move(temp);
    }

    return dependencies;
  }

  void initialize_grid_reader()
  {
    for (uint8_t symbol = 0; symbol != nof_symbols; ++symbol) {
      for (uint16_t k = 0, e = du_nof_prbs * NOF_SUBCARRIERS_PER_RB; k != e; ++k) {
        rg_reader_spy.write(
            resource_grid_reader_spy::expected_entry_t{0, symbol, k, (k > 200) ? cf_t{1, 1} : cf_t{1, 0}});
      }
    }
  }
};

static const std::array<ru_compression_params, 2> compr_params = {
    {{compression_type::none, 16}, {compression_type::BFP, 9}}};
static const std::array<std::vector<interval<unsigned>>, 2> segmented_prbs = {{{{0, 186}, {186, 273}}, {{0, 273}}}};

INSTANTIATE_TEST_SUITE_P(compression_params,
                         ofh_data_flow_uplane_downlink_data_impl_fixture,
                         ::testing::ValuesIn(compr_params));

TEST_P(ofh_data_flow_uplane_downlink_data_impl_fixture, calling_enqueue_section_type_1_message_success)
{
  data_flow_uplane_resource_grid_context context;
  context.port         = 0;
  context.sector       = 0;
  context.slot         = slot_point(0, 0, 0);
  context.eaxc         = 2;
  context.symbol_range = {0, 3};

  data_flow.enqueue_section_type_1_message(context, shared_rg_spy.get_grid());

  // Assert VLAN parameters.
  const ether::vlan_frame_params& vlan = vlan_builder->get_vlan_frame_params();
  ASSERT_TRUE(vlan_builder->has_build_vlan_frame_method_been_called());
  ASSERT_EQ(vlan_params.eth_type, vlan.eth_type);
  ASSERT_EQ(vlan_params.mac_dst_address, vlan.mac_dst_address);
  ASSERT_EQ(vlan_params.mac_src_address, vlan.mac_src_address);
  ASSERT_EQ(vlan_params.vlan_config->tci_vid, vlan.vlan_config->tci_vid);
  ASSERT_EQ(vlan_params.vlan_config->tci_pcp, vlan.vlan_config->tci_pcp);

  // Assert eCPRI parameters.
  ASSERT_TRUE(ecpri_builder->has_build_data_packet_method_been_called());
  ASSERT_FALSE(ecpri_builder->has_build_control_packet_method_been_called());
  // Assert there is only one packet per symbol.
  span<const ecpri::iq_data_parameters> data_params = ecpri_builder->get_data_parameters();
  ASSERT_EQ(data_params.size(), nof_symbols * segmented_prbs[static_cast<unsigned>(compr_params.type)].size());
  sequence_identifier_generator generator;
  for (const auto& param : data_params) {
    ASSERT_EQ(param.seq_id >> 8U, generator.generate(context.eaxc));
    ASSERT_EQ(param.pc_id, context.eaxc);
  }

  // Assert Open Fronthaul parameters.
  span<const uplane_message_params>      uplane_params = uplane_builder->get_uplane_params();
  const std::vector<interval<unsigned>>& seg_prbs      = segmented_prbs[static_cast<unsigned>(compr_params.type)];
  ASSERT_EQ(uplane_builder->nof_built_packets(), nof_symbols * seg_prbs.size());

  unsigned symbol_id = 0;
  unsigned prb_index = 0;
  for (const auto& param : uplane_params) {
    ASSERT_EQ(param.direction, data_direction::downlink);
    ASSERT_EQ(param.slot, context.slot);
    ASSERT_EQ(param.filter_index, filter_index_type::standard_channel_filter);
    ASSERT_EQ(param.start_prb, seg_prbs[prb_index].start());
    ASSERT_EQ(param.nof_prb, seg_prbs[prb_index].length());
    ASSERT_EQ(param.symbol_id, symbol_id);
    ASSERT_EQ(param.sect_type, section_type::type_1);
    ASSERT_EQ(param.compression_params.data_width, compr_params.data_width);
    ASSERT_EQ(param.compression_params.type, compr_params.type);

    // Check the symbols.
    std::vector<cbf16_t> iq_symbols(param.nof_prb * NOF_SUBCARRIERS_PER_RB, 0);
    rg_reader_spy.get(iq_symbols, 0, symbol_id, param.start_prb * NOF_SUBCARRIERS_PER_RB);

    ASSERT_EQ(span<const cbf16_t>(iq_symbols),
              uplane_builder->get_iq_data(symbol_id).subspan(param.start_prb * NOF_SUBCARRIERS_PER_RB,
                                                             param.nof_prb * NOF_SUBCARRIERS_PER_RB));

    // Increase the segmented PRB index.
    prb_index = (prb_index + 1) % seg_prbs.size();
    // Increase symbol index when all the PRBs for a symbol have been checked.
    if (prb_index == 0) {
      ++symbol_id;
    }
  }
}

TEST(ofh_data_flow_uplane_downlink_data_impl,
     frame_buffer_size_of_nof_prbs_plus_headers_size_generates_one_packet_per_symbol)
{
  data_flow_uplane_resource_grid_context context;
  context.port         = 0;
  context.sector       = 0;
  context.slot         = slot_point(0, 0, 0);
  context.eaxc         = 2;
  context.symbol_range = {0, 3};

  data_flow_uplane_downlink_data_impl_config config;

  config.ru_nof_prbs  = 273;
  config.compr_params = {compression_type::BFP, 9};

  ether::vlan_frame_params vlan_params = {{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x11},
                                          {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x22},
                                          ether::vlan_parameters{.tci_vid = 1},
                                          0xaabb};

  data_flow_uplane_downlink_data_impl_dependencies dependencies;
  dependencies.logger         = &ocudulog::fetch_basic_logger("TEST");
  dependencies.compressor_sel = std::make_unique<ofh::testing::iq_compressor_dummy>();

  unsigned prb_size   = 28;
  unsigned frame_size = prb_size * config.ru_nof_prbs;

  ofh_uplane_packet_builder_spy* uplane_builder;

  {
    auto temp = std::make_unique<ofh_uplane_packet_builder_spy>();
    frame_size += temp->get_header_size(config.compr_params).value();
    uplane_builder          = temp.get();
    dependencies.up_builder = std::move(temp);
  }
  {
    auto temp = std::make_unique<ether::testing::vlan_frame_builder_spy>(vlan_params);
    frame_size += temp->get_header_size().value();
    dependencies.eth_builder = std::move(temp);
  }
  {
    auto temp = std::make_unique<ecpri::testing::packet_builder_spy>();
    frame_size += temp->get_header_size(ecpri::message_type::iq_data).value();
    dependencies.ecpri_builder = std::move(temp);
  }

  dependencies.frame_pool = std::make_shared<ether::eth_frame_pool>(
      *dependencies.logger, units::bytes(frame_size), 2, ofh::message_type::user_plane, ofh::data_direction::downlink);

  resource_grid_reader_spy rg_reader_spy(1, context.symbol_range.length(), config.ru_nof_prbs);
  for (uint8_t symbol = 0; symbol != context.symbol_range.length(); ++symbol) {
    for (uint16_t k = 0, e = config.ru_nof_prbs * NOF_SUBCARRIERS_PER_RB; k != e; ++k) {
      rg_reader_spy.write(
          resource_grid_reader_spy::expected_entry_t{0, symbol, k, (k > 200) ? cf_t{1, 1} : cf_t{1, 0}});
    }
  }
  resource_grid_writer_spy rg_writer_spy;
  resource_grid_spy        rg_spy(rg_reader_spy, rg_writer_spy);
  shared_resource_grid_spy shared_rg_spy(rg_spy);

  data_flow_uplane_downlink_data_impl data_flow(config, std::move(dependencies));
  data_flow.enqueue_section_type_1_message(context, shared_rg_spy.get_grid());

  // Assert number of packets.
  ASSERT_EQ(uplane_builder->nof_built_packets(), context.symbol_range.length());
}

TEST(ofh_data_flow_uplane_downlink_data_impl, frame_buffer_size_of_nof_prbs_generates_two_packets_per_symbol)
{
  data_flow_uplane_resource_grid_context context;
  context.port         = 0;
  context.sector       = 0;
  context.slot         = slot_point(0, 0, 0);
  context.eaxc         = 2;
  context.symbol_range = {0, 3};

  data_flow_uplane_downlink_data_impl_config config;

  config.ru_nof_prbs  = 273;
  config.compr_params = {compression_type::BFP, 9};

  ether::vlan_frame_params vlan_params = {{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x11},
                                          {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x22},
                                          ether::vlan_parameters{.tci_vid = 1},
                                          0xaabb};

  data_flow_uplane_downlink_data_impl_dependencies dependencies;
  dependencies.logger         = &ocudulog::fetch_basic_logger("TEST");
  dependencies.compressor_sel = std::make_unique<ofh::testing::iq_compressor_dummy>();

  unsigned prb_size  = 28;
  unsigned prbs_size = prb_size * config.ru_nof_prbs;

  dependencies.frame_pool = std::make_shared<ether::eth_frame_pool>(
      *dependencies.logger, units::bytes(prbs_size), 2, message_type::user_plane, data_direction::downlink);
  ofh_uplane_packet_builder_spy* uplane_builder;

  {
    auto temp               = std::make_unique<ofh_uplane_packet_builder_spy>();
    uplane_builder          = temp.get();
    dependencies.up_builder = std::move(temp);
  }
  {
    auto temp                = std::make_unique<ether::testing::vlan_frame_builder_spy>(vlan_params);
    dependencies.eth_builder = std::move(temp);
  }
  {
    auto temp                  = std::make_unique<ecpri::testing::packet_builder_spy>();
    dependencies.ecpri_builder = std::move(temp);
  }
  resource_grid_reader_spy rg_reader_spy(1, context.symbol_range.length(), config.ru_nof_prbs);
  for (uint8_t symbol = 0; symbol != context.symbol_range.length(); ++symbol) {
    for (uint16_t k = 0, e = config.ru_nof_prbs * NOF_SUBCARRIERS_PER_RB; k != e; ++k) {
      rg_reader_spy.write(
          resource_grid_reader_spy::expected_entry_t{0, symbol, k, (k > 200) ? cf_t{1, 1} : cf_t{1, 0}});
    }
  }
  resource_grid_writer_spy rg_writer_spy;
  resource_grid_spy        rg_spy(rg_reader_spy, rg_writer_spy);
  shared_resource_grid_spy shared_rg_spy(rg_spy);

  data_flow_uplane_downlink_data_impl data_flow(config, std::move(dependencies));
  data_flow.enqueue_section_type_1_message(context, shared_rg_spy.get_grid());

  // Assert number of packets. As the packet should not fit in the frame, check that it generated 2 packets per symbol.
  ASSERT_EQ(uplane_builder->nof_built_packets(), context.symbol_range.length() * 2);
}

namespace {

/// Resource-grid reader spy that reports a device-visible grid, so that the slot-burst paths are taken.
class device_grid_reader_spy : public resource_grid_reader_spy
{
public:
  using resource_grid_reader_spy::resource_grid_reader_spy;

  bool supports_device_grid_reading() const override { return true; }
};

/// User-Plane builder spy for the region path: lays out 8 header bytes followed by the payload of each message.
class uplane_header_builder_spy : public uplane_message_builder
{
public:
  static constexpr unsigned header_size = 8;

  struct batch_call {
    unsigned              first_symbol;
    unsigned              nof_symbols;
    uplane_message_params params;
  };

  units::bytes get_header_size(const ru_compression_params& params) const override { return units::bytes(header_size); }

  unsigned build_message(span<uint8_t> buffer, span<const cbf16_t> grid, const uplane_message_params& params) override
  {
    ++nof_message_builds;
    return 0;
  }

  bool build_symbol_batch_message_headers(span<span<uint8_t>>          buffers,
                                          unsigned                     first_symbol,
                                          unsigned                     nof_symbols,
                                          const uplane_message_params& params,
                                          span<span<uint8_t>>          payloads,
                                          span<unsigned>               bytes_written) override
  {
    const unsigned payload_size = params.nof_prb * get_compressed_prb_size(params.compression_params).value();
    for (unsigned i = 0, e = buffers.size(); i != e; ++i) {
      if (buffers[i].size() < header_size + payload_size) {
        return false;
      }
      payloads[i]      = buffers[i].subspan(header_size, payload_size);
      bytes_written[i] = header_size + payload_size;
    }
    batch_calls.push_back({first_symbol, nof_symbols, params});
    all_payloads.insert(all_payloads.end(), payloads.begin(), payloads.end());
    return true;
  }

  std::vector<batch_call>    batch_calls;
  std::vector<span<uint8_t>> all_payloads;
  unsigned                   nof_message_builds = 0;
};

/// Compressor spy for the region path; records the regions and the per-fragment fallback calls.
class region_compressor_spy : public iq_compressor
{
public:
  struct fallback_call {
    std::vector<span<uint8_t>> buffers;
    unsigned                   first_port;
    unsigned                   nof_ports;
    unsigned                   first_symbol;
    unsigned                   nof_symbols;
    unsigned                   start_prb;
    unsigned                   nof_prbs;
  };

  region_compressor_spy(bool supports_, bool succeeds_) : supports(supports_), succeeds(succeeds_) {}

  void compress(span<uint8_t> buffer, span<const cbf16_t> iq_data, const ru_compression_params& params) override {}

  bool supports_device_grid_regions(const ru_compression_params& params) const override { return supports; }

  bool compress_device_grid_regions(span<const device_grid_compression_region> regions,
                                    const resource_grid_reader&                grid,
                                    const ru_compression_params&               params) override
  {
    region_calls.emplace_back(regions.begin(), regions.end());
    return succeeds;
  }

  bool compress_device_symbol_batch_to_buffers(span<span<uint8_t>>          buffers,
                                               const resource_grid_reader&  grid,
                                               unsigned                     first_port,
                                               unsigned                     nof_ports,
                                               unsigned                     first_symbol,
                                               unsigned                     nof_symbols,
                                               unsigned                     start_prb,
                                               unsigned                     nof_prbs,
                                               const ru_compression_params& params) override
  {
    fallback_calls.push_back(
        {{buffers.begin(), buffers.end()}, first_port, nof_ports, first_symbol, nof_symbols, start_prb, nof_prbs});
    return true;
  }

  const bool                                               supports;
  const bool                                               succeeds;
  std::vector<std::vector<device_grid_compression_region>> region_calls;
  std::vector<fallback_call>                               fallback_calls;
};

/// Region-path test bench: 2 ports, 3 symbols, 273 PRBs of BFP 9, frames holding 150 PRBs (two fragments per symbol).
struct region_path_bench {
  static constexpr unsigned nof_ports   = 2;
  static constexpr unsigned nof_symbols = 3;
  static constexpr unsigned nof_prbs    = 273;
  static constexpr unsigned frame_prbs  = 150;

  region_path_bench(bool supports, bool succeeds) :
    rg_reader(nof_ports, nof_symbols, nof_prbs), rg(rg_reader, rg_writer), shared_rg(rg)
  {
    data_flow_uplane_downlink_data_impl_config config;
    config.ru_nof_prbs  = nof_prbs;
    config.compr_params = {compression_type::BFP, 9};

    data_flow_uplane_downlink_data_impl_dependencies dependencies;
    dependencies.logger         = &ocudulog::fetch_basic_logger("TEST");
    auto compr                  = std::make_unique<region_compressor_spy>(supports, succeeds);
    compressor                  = compr.get();
    dependencies.compressor_sel = std::move(compr);
    auto up                     = std::make_unique<uplane_header_builder_spy>();
    up_builder                  = up.get();
    dependencies.up_builder     = std::move(up);
    auto eth                    = std::make_unique<ether::testing::vlan_frame_builder_spy>(ether::vlan_frame_params{
                           {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x11}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x22}, {}, 0xaabb});
    auto ecpri                  = std::make_unique<ecpri::testing::packet_builder_spy>();
    ecpri_builder               = ecpri.get();
    const unsigned frame_size   = frame_prbs * get_compressed_prb_size(config.compr_params).value() +
                                uplane_header_builder_spy::header_size + eth->get_header_size().value() +
                                ecpri->get_header_size(ecpri::message_type::iq_data).value();
    dependencies.eth_builder   = std::move(eth);
    dependencies.ecpri_builder = std::move(ecpri);
    dependencies.frame_pool    = std::make_shared<ether::eth_frame_pool>(
        *dependencies.logger, units::bytes(frame_size), 2, message_type::user_plane, data_direction::downlink);
    data_flow.emplace(config, std::move(dependencies));
  }

  void run()
  {
    data_flow_uplane_resource_grid_context context;
    context.sector       = 0;
    context.slot         = slot_point(0, 0, 0);
    context.symbol_range = {0, nof_symbols};
    data_flow->enqueue_section_type_1_messages(context, 0, eaxcs, shared_rg.get_grid());
  }

  const std::array<unsigned, nof_ports>              eaxcs = {4, 5};
  device_grid_reader_spy                             rg_reader;
  resource_grid_writer_spy                           rg_writer;
  resource_grid_spy                                  rg;
  shared_resource_grid_spy                           shared_rg;
  region_compressor_spy*                             compressor    = nullptr;
  uplane_header_builder_spy*                         up_builder    = nullptr;
  ecpri::testing::packet_builder_spy*                ecpri_builder = nullptr;
  std::optional<data_flow_uplane_downlink_data_impl> data_flow;
};

} // namespace

TEST(ofh_data_flow_uplane_downlink_data_impl, slot_regions_compress_every_fragment_with_one_call)
{
  region_path_bench bench(true, true);
  bench.run();

  // Headers of both fragments built as symbol batches; every fragment, symbol and port compressed in one call.
  ASSERT_EQ(bench.up_builder->batch_calls.size(), 2);
  const interval<unsigned> fragments[] = {{0, region_path_bench::frame_prbs},
                                          {region_path_bench::frame_prbs, region_path_bench::nof_prbs}};
  for (unsigned i_fragment = 0; i_fragment != 2; ++i_fragment) {
    const auto& call = bench.up_builder->batch_calls[i_fragment];
    EXPECT_EQ(call.first_symbol, 0);
    EXPECT_EQ(call.nof_symbols, region_path_bench::nof_symbols);
    EXPECT_EQ(call.params.start_prb, fragments[i_fragment].start());
    EXPECT_EQ(call.params.nof_prb, fragments[i_fragment].length());
  }
  ASSERT_EQ(bench.compressor->region_calls.size(), 1);
  EXPECT_TRUE(bench.compressor->fallback_calls.empty());
  EXPECT_EQ(bench.up_builder->nof_message_builds, 0);

  // Regions ordered by fragment, symbol and port, each writing the payload of its own message.
  const std::vector<device_grid_compression_region>& regions = bench.compressor->region_calls.front();
  ASSERT_EQ(regions.size(), 2 * region_path_bench::nof_symbols * region_path_bench::nof_ports);
  ASSERT_EQ(bench.up_builder->all_payloads.size(), regions.size());
  for (unsigned i = 0, e = regions.size(); i != e; ++i) {
    const unsigned i_fragment = i / (region_path_bench::nof_symbols * region_path_bench::nof_ports);
    EXPECT_EQ(regions[i].port, i % region_path_bench::nof_ports) << i;
    EXPECT_EQ(regions[i].symbol, (i / region_path_bench::nof_ports) % region_path_bench::nof_symbols) << i;
    EXPECT_EQ(regions[i].start_prb, fragments[i_fragment].start()) << i;
    EXPECT_EQ(regions[i].nof_prbs, fragments[i_fragment].length()) << i;
    EXPECT_EQ(regions[i].output.data(), bench.up_builder->all_payloads[i].data()) << i;
    EXPECT_EQ(regions[i].output.size(), bench.up_builder->all_payloads[i].size()) << i;
  }

  // One eCPRI / Ethernet message per region, carrying its port's eAxC.
  span<const ecpri::iq_data_parameters> data_params = bench.ecpri_builder->get_data_parameters();
  ASSERT_EQ(data_params.size(), regions.size());
  for (unsigned i = 0, e = data_params.size(); i != e; ++i) {
    EXPECT_EQ(data_params[i].pc_id, bench.eaxcs[i % region_path_bench::nof_ports]) << i;
  }
}

TEST(ofh_data_flow_uplane_downlink_data_impl, slot_regions_failure_falls_back_per_fragment)
{
  region_path_bench bench(true, false);
  bench.run();

  // The region call fails: each fragment is compressed as a symbol batch into the same payloads.
  ASSERT_EQ(bench.compressor->region_calls.size(), 1);
  ASSERT_EQ(bench.compressor->fallback_calls.size(), 2);
  const unsigned messages_per_fragment = region_path_bench::nof_symbols * region_path_bench::nof_ports;
  for (unsigned i_fragment = 0; i_fragment != 2; ++i_fragment) {
    const auto& call = bench.compressor->fallback_calls[i_fragment];
    EXPECT_EQ(call.first_port, 0);
    EXPECT_EQ(call.nof_ports, region_path_bench::nof_ports);
    EXPECT_EQ(call.first_symbol, 0);
    EXPECT_EQ(call.nof_symbols, region_path_bench::nof_symbols);
    EXPECT_EQ(call.start_prb, i_fragment * region_path_bench::frame_prbs);
    EXPECT_EQ(call.nof_prbs,
              i_fragment == 0 ? region_path_bench::frame_prbs
                              : region_path_bench::nof_prbs - region_path_bench::frame_prbs);
    ASSERT_EQ(call.buffers.size(), messages_per_fragment);
    for (unsigned i = 0; i != messages_per_fragment; ++i) {
      EXPECT_EQ(call.buffers[i].data(), bench.up_builder->all_payloads[i_fragment * messages_per_fragment + i].data());
    }
  }
  EXPECT_EQ(bench.ecpri_builder->get_data_parameters().size(), 2 * messages_per_fragment);
  EXPECT_EQ(bench.up_builder->nof_message_builds, 0);
}

TEST(ofh_data_flow_uplane_downlink_data_impl, slot_regions_unused_when_the_compressor_does_not_support_them)
{
  region_path_bench bench(false, true);
  bench.run();

  // The previous paths build the messages one by one (this builder has no symbol-batch build).
  EXPECT_TRUE(bench.compressor->region_calls.empty());
  EXPECT_TRUE(bench.up_builder->batch_calls.empty());
  EXPECT_EQ(bench.up_builder->nof_message_builds, 2 * region_path_bench::nof_symbols * region_path_bench::nof_ports);
}
