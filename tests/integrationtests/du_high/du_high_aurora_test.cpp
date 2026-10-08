/// \file
/// \brief Integration test: AURORA hooks across scheduler / MAC / RLC / F1-U are fed by a real DU-high UE flow.

#include "lib/aurora/aurora_state.h"
#include "tests/integrationtests/du_high/test_utils/du_high_env_simulator.h"
#include "tests/test_doubles/pdcp/pdcp_pdu_generator.h"
#include "ocudu/aurora/aurora.h"
#include <gtest/gtest.h>
#include <unistd.h>

using namespace ocudu;
using namespace odu;

namespace {

class du_high_aurora_tester : public du_high_env_simulator, public testing::Test
{
protected:
  du_high_aurora_tester()
  {
    aurora::config cfg;
    cfg.enabled                = true;
    cfg.type                   = aurora::ran_type::du;
    cfg.collection_interval    = std::chrono::milliseconds{3600 * 1000};
    cfg.shm_key                = 0x7d000000U | (static_cast<uint32_t>(::getpid()) & 0xffffffU);
    cfg.thread_metrics_enabled = false;
    cfg.anomaly_enabled        = false;
    EXPECT_EQ(aurora::start(cfg), 0);
    snapshot = std::make_unique<aurora_metrics>();
    aurora::detail::init_shm_content(*snapshot, cfg);
  }

  ~du_high_aurora_tester() override { aurora::stop(); }

  const aurora_metrics& publish()
  {
    aurora::detail::publish_interval(*snapshot, *aurora::detail::state.load());
    return *snapshot;
  }

  std::unique_ptr<aurora_metrics> snapshot;
};

} // namespace

TEST_F(du_high_aurora_tester, ue_attach_and_dl_traffic_populate_aurora_metrics)
{
  rnti_t rnti = to_rnti(0x4601);
  ASSERT_TRUE(add_ue(rnti));
  ASSERT_TRUE(run_rrc_setup(rnti));
  ASSERT_TRUE(run_ue_context_setup(rnti));
  // Drop the attach interval.
  publish();

  // DL traffic through F1-U -> RLC -> MAC.
  const unsigned nof_pdcp_pdus = 50, pdcp_pdu_size = 128;
  for (unsigned i = 0; i != nof_pdcp_pdus; ++i) {
    nru_dl_message f1u_pdu{.t_pdu = test_helpers::create_pdcp_pdu(
                               pdcp_sn_size::size12bits, /* is_srb = */ false, i, pdcp_pdu_size, i & 0xffU)};
    for (auto& bearer : cu_up_sim.bearers) {
      bearer.second.rx_notifier->on_new_pdu(f1u_pdu);
    }
  }
  for (unsigned i = 0; i != 200; ++i) {
    run_slot();
  }

  const aurora_metrics& m    = publish();
  const unsigned        slot = aurora::detail::ue_slot(0);

  // Scheduler / FAPI (Groups 5, 7, 11, 12).
  EXPECT_GT(m.fapi_dl_total_pdsch_count, 0U);
  EXPECT_GT(m.fapi_dl_total_pdsch_size, 0U);
  EXPECT_GT(m.dl_fapi_prb_max, 0U);
  EXPECT_EQ(m.mac_ue_stats[slot].rnti, 0x4601U);
  EXPECT_GT(m.mac_ue_stats[slot].dl_aggr_tbs, 0U);
  EXPECT_GT(m.mac_ue_stats[slot].dl_aggr_prb, 0U);
  EXPECT_GT(m.dl_prb_utilization_pct, 0.0F);
  EXPECT_EQ(m.active_ue_count, 1U);
  EXPECT_GT(m.sched_loop_max_us, 0U);
  EXPECT_EQ(m.kpm_metrics[0].value, 1U);

  // MAC <-> RLC (Group 1, Group 12).
  EXPECT_GT(m.rlc_mac_size, uint64_t(nof_pdcp_pdus) * pdcp_pdu_size / 2);
  EXPECT_GT(m.hist_rlc_mac.count, 0U);
  EXPECT_GT(m.mac_ue_stats[slot].dl_aggr_bytes_sdus, 0U);
  EXPECT_GT(m.hist_mac_dl_bo.count, 0U);

  // F1-U -> RLC (Group 1).
  EXPECT_EQ(m.f1u_rlc_size, uint64_t(nof_pdcp_pdus) * pdcp_pdu_size);
  EXPECT_EQ(m.hist_f1u_rlc.count, nof_pdcp_pdus);

  // RLC DRB1 (Group 13).
  const aurora_rlc_radio_bearer_stats_t& drb1 = m.rlc_rb_stats[slot][aurora::drb_rb_index(1)];
  EXPECT_EQ(drb1.rnti, 0x4601U);
  EXPECT_EQ(drb1.rbid, 1);
  EXPECT_EQ(drb1.txsdu_pkts, nof_pdcp_pdus);
  EXPECT_EQ(drb1.txsdu_bytes, uint64_t(nof_pdcp_pdus) * pdcp_pdu_size);
  EXPECT_GT(drb1.txpdu_pkts, 0U);
  EXPECT_GT(drb1.txpdu_bytes, drb1.txsdu_bytes / 2);
  // OCUDU extension: CSI reports decoded by the scheduler during the traffic phase.
  EXPECT_EQ(m.ext.version, unsigned(AURORA_EXT_VERSION));
  EXPECT_EQ(m.ext.csi[slot].rnti, 0x4601U);
  EXPECT_GT(m.ext.csi[slot].nof_reports, 0U);

  // SRB1 carried the RRC setup / context setup signalling.
  EXPECT_NE(m.rlc_rb_stats[slot][aurora::srb_rb_index(1)].rnti, 0U);
}
