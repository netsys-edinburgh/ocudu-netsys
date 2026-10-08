// Unit tests of the AURORA core: hook -> accumulator -> shared-memory publication semantics.
// The publisher period is set to one hour so that the tests drive publish_interval() deterministically.

#include "lib/aurora/aurora_state.h"
#include "ocudu/aurora/aurora.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::aurora;

namespace {

class aurora_core_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    config cfg;
    cfg.enabled                = true;
    cfg.type                   = ran_type::gnb;
    cfg.collection_interval    = std::chrono::milliseconds{3600 * 1000};
    cfg.shm_key                = 0x7a000000U | (static_cast<uint32_t>(::getpid()) & 0xffffffU);
    cfg.thread_metrics_enabled = false;
    cfg.anomaly_enabled        = false;
    ASSERT_EQ(start(cfg), 0);
    ASSERT_TRUE(is_active());
    m = std::make_unique<aurora_metrics>();
    detail::init_shm_content(*m, cfg);
  }

  void TearDown() override { stop(); }

  const aurora_metrics& publish()
  {
    detail::publish_interval(*m, *detail::state.load());
    return *m;
  }

  std::unique_ptr<aurora_metrics> m;
};

grant_info make_grant(unsigned ue, uint16_t rnti, unsigned mcs, unsigned prbs, unsigned tbs, unsigned retxs = 0)
{
  grant_info g;
  g.ue_index  = ue;
  g.rnti      = rnti;
  g.mcs       = mcs;
  g.nof_prbs  = prbs;
  g.tbs_bytes = tbs;
  g.nof_retxs = retxs;
  return g;
}

} // namespace

TEST(aurora_hist, byte_bins_match_oai_boundaries)
{
  aurora_hist_t h{};
  for (uint32_t v : {0U, 63U, 64U, 127U, 128U, 1024U, 524287U, 524288U, 1048576U, 0xffffffffU}) {
    aurora_hist_record(&h, v);
  }
  EXPECT_EQ(h.bins[0], 2U);
  EXPECT_EQ(h.bins[1], 2U);
  EXPECT_EQ(h.bins[2], 1U);
  EXPECT_EQ(h.bins[5], 1U);
  EXPECT_EQ(h.bins[13], 1U);
  EXPECT_EQ(h.bins[14], 1U);
  EXPECT_EQ(h.bins[15], 2U);
  EXPECT_EQ(h.count, 10U);
  EXPECT_EQ(h.min_val, 0U);
  EXPECT_EQ(h.max_val, 0xffffffffU);
}

TEST(aurora_hist, shift_bins)
{
  aurora_hist_t h{};
  aurora_hist_record_shift(&h, 27, 2);
  aurora_hist_record_shift(&h, 1000, 5);
  EXPECT_EQ(h.bins[6], 1U);
  EXPECT_EQ(h.bins[15], 1U);
}

TEST(aurora_lifecycle, hooks_are_noops_when_not_started)
{
  ASSERT_FALSE(is_active());
  record_flow(flow::bhtx_in, 100);
  on_pdsch_grant(make_grant(0, 0x4601, 10, 10, 100));
  rlc_add(0, 4, rlc_counter::txsdu_pkts);
  SUCCEED();
}

TEST_F(aurora_core_test, shm_segment_is_created_with_role)
{
  aurora_metrics* attached = aurora_get_metrics_by_key(static_cast<key_t>(detail::state.load()->cfg.shm_key));
  ASSERT_NE(attached, nullptr);
  EXPECT_EQ(attached->ran_type, RAN_GNB);
  EXPECT_EQ(attached->collection_interval_ms, 3600U * 1000U);
  EXPECT_EQ(attached->dl_fapi_mcs_min, 0xff);
}

TEST_F(aurora_core_test, traffic_flow_bytes_and_histogram_are_interval_deltas)
{
  record_flow(flow::bhtx_in, 100);
  record_flow(flow::bhtx_in, 1500);
  record_flow(flow::mac_rlc, 64);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.bhtx_in_size, 1600U);
  EXPECT_EQ(r.hist_bhtx_in.count, 2U);
  EXPECT_EQ(r.hist_bhtx_in.min_val, 100U);
  EXPECT_EQ(r.hist_bhtx_in.max_val, 1500U);
  EXPECT_EQ(r.hist_bhtx_in.sum, 1600U);
  EXPECT_EQ(r.hist_bhtx_in.bins[1], 1U);
  EXPECT_EQ(r.hist_bhtx_in.bins[5], 1U);
  EXPECT_EQ(r.mac_rlc_size, 64U);

  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.bhtx_in_size, 0U);
  EXPECT_EQ(r2.hist_bhtx_in.count, 0U);
  EXPECT_EQ(r2.hist_bhtx_in.min_val, 0U);
}

TEST_F(aurora_core_test, dl_harq_counts_nack_and_dtx_and_max_consecutive)
{
  on_ue_created(1, 0x4601);
  on_dl_harq_feedback(1, 0x4601, 3, harq_ack_status::nack);
  on_dl_harq_feedback(1, 0x4601, 3, harq_ack_status::dtx);
  on_dl_harq_feedback(1, 0x4601, 3, harq_ack_status::ack);
  on_dl_harq_feedback(1, 0x4601, 3, harq_ack_status::nack);
  on_dl_harq_feedback(1, 0x4601, 5, harq_ack_status::ack);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.dl_harq_ack, 2U);
  EXPECT_EQ(r.dl_harq_nack, 3U);
  EXPECT_EQ(r.dl_harq_total, 5U);
  EXPECT_DOUBLE_EQ(r.dl_harq_loss_rate, 0.6);
  EXPECT_EQ(r.dl_harq_max_cons, 2U);
  EXPECT_FLOAT_EQ(r.mac_ue_stats[1].dl_bler, 0.6F);
}

TEST_F(aurora_core_test, ul_crc_and_sinr)
{
  on_ue_created(2, 0x4602);
  on_ul_crc(2, 0x4602, true, 12.5F);
  on_ul_crc(2, 0x4602, false, std::nullopt);
  on_ul_crc(2, 0x4602, true, std::nullopt);
  on_ul_dtx(2);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.ul_crc_total, 3U);
  EXPECT_EQ(r.ul_crc_loss, 1U);
  EXPECT_NEAR(r.ul_crc_loss_rate, 1.0 / 3, 1e-9);
  EXPECT_FLOAT_EQ(r.mac_ue_stats[2].pusch_snr, 12.5F);
  EXPECT_EQ(r.mac_ue_stats[2].ulsch_dtx, 1U);
  EXPECT_EQ(r.mac_ue_stats[2].rnti, 0x4602U);
}

TEST_F(aurora_core_test, fapi_dl_ul_aggregates_histograms_and_per_ue_snapshot)
{
  on_ue_created(3, 0x4603);
  on_pdsch_grant(make_grant(3, 0x4603, 10, 5, 100));
  on_pdsch_grant(make_grant(3, 0x4603, 20, 40, 300, 1));
  on_pusch_grant(make_grant(3, 0x4603, 7, 12, 50));
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.fapi_dl_total_pdsch_count, 2U);
  EXPECT_EQ(r.fapi_dl_total_pdsch_size, 400U);
  EXPECT_FLOAT_EQ(r.fapi_dl_avg_pdsch, 200.0F);
  EXPECT_EQ(r.dl_fapi_mcs_min, 10);
  EXPECT_EQ(r.dl_fapi_mcs_max, 20);
  EXPECT_FLOAT_EQ(r.dl_fapi_mcs_avg, 15.0F);
  EXPECT_EQ(r.dl_fapi_prb_min, 5U);
  EXPECT_EQ(r.dl_fapi_prb_max, 40U);
  EXPECT_FLOAT_EQ(r.dl_fapi_prb_avg, 22.5F);
  EXPECT_EQ(r.dl_fapi_tbs_min, 100U);
  EXPECT_EQ(r.dl_fapi_tbs_max, 300U);
  EXPECT_FLOAT_EQ(r.dl_fapi_tbs_avg, 200.0F);
  EXPECT_EQ(r.hist_dl_fapi_mcs.bins[2], 1U);
  EXPECT_EQ(r.hist_dl_fapi_mcs.bins[5], 1U);
  EXPECT_EQ(r.hist_dl_fapi_prb.bins[0], 1U);
  EXPECT_EQ(r.hist_dl_fapi_prb.bins[1], 1U);
  EXPECT_EQ(r.fapi_ul_total_pusch_count, 1U);
  EXPECT_EQ(r.ul_fapi_mcs_min, 7);
  EXPECT_EQ(r.ul_fapi_tbs_max, 50U);
  EXPECT_EQ(r.fapi_ue_count, 1U);
  EXPECT_EQ(r.fapi_ue_metrics[3].rnti, 0x4603);
  EXPECT_EQ(r.fapi_ue_metrics[3].dl_mcs, 20);
  EXPECT_EQ(r.fapi_ue_metrics[3].dl_prb, 40U);
  EXPECT_EQ(r.fapi_ue_metrics[3].ul_tbs, 50U);
  const aurora_mac_ue_stats_t& u = r.mac_ue_stats[3];
  EXPECT_EQ(u.dl_aggr_tbs, 400U);
  EXPECT_EQ(u.dl_aggr_prb, 45U);
  EXPECT_EQ(u.dl_aggr_retx_prb, 40U);
  EXPECT_EQ(u.dl_harq_rounds[0], 1U);
  EXPECT_EQ(u.dl_harq_rounds[1], 1U);
  EXPECT_EQ(u.dl_num_harq, 2U);

  // Empty interval: OAI reset sentinels.
  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.fapi_dl_total_pdsch_count, 0U);
  EXPECT_EQ(r2.dl_fapi_mcs_min, 0xff);
  EXPECT_EQ(r2.dl_fapi_prb_min, 0xffffffffU);
  EXPECT_EQ(r2.fapi_ue_metrics[3].rnti, 0);
  EXPECT_EQ(r2.mac_ue_stats[3].dl_starvation_events, 1U);
}

TEST_F(aurora_core_test, cross_ue_aggregates_kpm_and_rc_cell)
{
  on_ue_created(0, 0x4600);
  on_ue_created(1, 0x4601);
  on_ul_crc(0, 0x4600, true, 10.0F);
  on_ul_crc(1, 0x4601, true, 30.0F);
  csi_info c0;
  c0.wb_cqi      = 7;
  c0.nof_rs         = 1;
  c0.l1_rsrp_dbm[0] = -90;
  csi_info c1;
  c1.wb_cqi      = 13;
  c1.nof_rs         = 1;
  c1.l1_rsrp_dbm[0] = -70;
  on_csi_report(0, c0);
  on_csi_report(1, c1);
  on_bsr(0, 1000);
  on_bsr(1, 3000);
  on_pdsch_grant(make_grant(0, 0x4600, 10, 25, 1000));
  on_pdsch_grant(make_grant(1, 0x4601, 10, 25, 3000));
  on_scheduler_slot(1, std::chrono::microseconds{40}, 50, true, false);
  on_scheduler_slot(1, std::chrono::microseconds{60}, 50, true, true);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.active_ue_count, 2U);
  EXPECT_FLOAT_EQ(r.sinr_min, 10.0F);
  EXPECT_FLOAT_EQ(r.sinr_max, 30.0F);
  EXPECT_FLOAT_EQ(r.sinr_avg, 20.0F);
  EXPECT_EQ(r.csi_min, 7);
  EXPECT_EQ(r.csi_max, 13);
  EXPECT_FLOAT_EQ(r.csi_avg, 10.0F);
  EXPECT_EQ(r.bsr_min, 1000U);
  EXPECT_EQ(r.bsr_max, 3000U);
  EXPECT_FLOAT_EQ(r.bsr_avg, 2000.0F);
  EXPECT_FLOAT_EQ(r.sched_loop_avg_us, 50.0F);
  EXPECT_EQ(r.sched_loop_max_us, 60U);
  EXPECT_FLOAT_EQ(r.dl_prb_utilization_pct, 50.0F);
  EXPECT_EQ(r.ue_attach_count, 2U);

  ASSERT_EQ(r.kpm_metric_count, 16U);
  EXPECT_EQ(r.kpm_metrics[0].measurement_id, 1U);
  EXPECT_STREQ(r.kpm_metrics[0].name, "RRC.ConnNumber");
  EXPECT_EQ(r.kpm_metrics[0].value, 2U);
  EXPECT_DOUBLE_EQ(r.kpm_metrics[1].value_real, 2000.0);
  EXPECT_EQ(r.kpm_metrics[5].value, 50U);
  EXPECT_DOUBLE_EQ(r.kpm_metrics[7].value_real, 10.0);
  EXPECT_DOUBLE_EQ(r.kpm_metrics[11].value_real, -80.0);
  EXPECT_EQ(r.kpm_metrics[13].value, 50U);
  EXPECT_STREQ(r.kpm_metrics[15].name, "L1M.UL-dtxRatio");

  EXPECT_EQ(r.rc_cell_count, 1U);
  EXPECT_EQ(r.rc_metrics[0].cell_id, 1U);
  EXPECT_DOUBLE_EQ(r.rc_metrics[0].rsrp_avg, -80.0);
  EXPECT_DOUBLE_EQ(r.rc_metrics[0].sinr_avg, 20.0);
  EXPECT_EQ(r.rc_metrics[0].interference_level, 0U);
  EXPECT_EQ(r.rc_metrics[0].cell_load_percent, 50U);
}

TEST_F(aurora_core_test, ue_removal_frees_slot)
{
  on_ue_created(4, 0x4604);
  rlc_bearer_created(4, drb_rb_index(1), rlc_mode::am);
  on_ue_removed(4);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.mac_ue_stats[4].rnti, 0U);
  EXPECT_EQ(r.rlc_rb_stats[4][4].rnti, 0U);
  EXPECT_EQ(r.ue_detach_count, 1U);
  EXPECT_EQ(r.active_ue_count, 0U);
}

TEST_F(aurora_core_test, rlc_bearer_counters_and_gauges)
{
  on_ue_created(5, 0x4605);
  const unsigned rb = drb_rb_index(1);
  EXPECT_EQ(rb, 4U);
  rlc_bearer_created(5, rb, rlc_mode::um);
  rlc_add(5, rb, rlc_counter::txsdu_pkts, 3);
  rlc_add(5, rb, rlc_counter::txsdu_bytes, 3000);
  rlc_add(5, rb, rlc_counter::txpdu_dd_pkts, 1);
  rlc_set(5, rb, rlc_gauge::txbuf_occ_bytes, 2048);
  rlc_set(5, rb, rlc_gauge::txsdu_wt_us, 777);
  const aurora_metrics& r = publish();
  const aurora_rlc_radio_bearer_stats_t& s = r.rlc_rb_stats[5][rb];
  EXPECT_EQ(s.rnti, 0x4605U);
  EXPECT_EQ(s.rbid, 1);
  EXPECT_EQ(s.mode, 1U);
  EXPECT_EQ(s.txsdu_pkts, 3U);
  EXPECT_EQ(s.txsdu_bytes, 3000U);
  EXPECT_EQ(s.txbuf_occ_bytes, 2048U);
  EXPECT_EQ(s.txsdu_wt_us, 777U);
  EXPECT_NEAR(r.kpm_metrics[12].value_real, 1.0 / 3, 1e-9);

  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.rlc_rb_stats[5][rb].txsdu_pkts, 0U);
  EXPECT_EQ(r2.rlc_rb_stats[5][rb].txbuf_occ_bytes, 2048U) << "gauges persist across intervals";
}

TEST_F(aurora_core_test, pdcp_counters_latency_and_kpm_bitrate)
{
  pdcp_bearer_created(7, drb_rb_index(2), rlc_mode::am);
  pdcp_add(7, drb_rb_index(2), pdcp_counter::txsdu_bytes, 1000);
  pdcp_add(7, drb_rb_index(2), pdcp_counter::rxsdu_bytes, 400);
  pdcp_add(7, drb_rb_index(2), pdcp_counter::rxpdu_oo_pkts, 2);
  pdcp_tx_proc_latency(7, drb_rb_index(2), 3000);
  pdcp_tx_proc_latency(7, drb_rb_index(2), 5000);
  pdcp_tx_sojourn(7, drb_rb_index(2), 6000);
  const aurora_metrics& r = publish();
  const aurora_pdcp_radio_bearer_stats_t& p = r.pdcp_rb_stats[7][5];
  EXPECT_EQ(p.rnti, 8U);
  EXPECT_EQ(p.rbid, 2);
  EXPECT_EQ(p.txsdu_bytes, 1000U);
  EXPECT_EQ(p.rxpdu_oo_pkts, 2U);
  EXPECT_EQ(p.tx_proc_latency_us, 4U);
  EXPECT_EQ(p.txsdu_wt_us, 2U);
  EXPECT_EQ(r.kpm_metrics[8].value, 1000U);
  EXPECT_EQ(r.kpm_metrics[9].value, 400U);

  pdcp_bearer_removed(7, drb_rb_index(2));
  EXPECT_EQ(publish().pdcp_rb_stats[7][5].rnti, 0U);
}

TEST_F(aurora_core_test, gtp_tunnel_counters_loss_and_unknown_teid)
{
  ASSERT_EQ(gtp_tunnel_created(9, 0x100, 0x200), 0);
  EXPECT_EQ(gtp_tunnel_created(9, 0x101, 0x201), 1);
  gtp_tunnel_removed(9, 1);
  gtp_set_qfi(9, 0, 5);
  gtp_add(9, 0, gtp_counter::tx_pkts, 9);
  gtp_add(9, 0, gtp_counter::tx_err_pkts, 1);
  gtp_add(9, 0, gtp_counter::rx_pkts, 4);
  gtp_add(9, 0, gtp_counter::rx_bytes, 400);
  gtp_unknown_teid();
  gtp_tx_latency(9, 0, 2000);
  const aurora_metrics& r = publish();
  const aurora_gtp_tunnel_stats_t& t = r.gtp_tunnel_stats[9][0];
  EXPECT_EQ(t.rnti, 10U);
  EXPECT_EQ(t.teidgnb, 0x100U);
  EXPECT_EQ(t.teidupf, 0x200U);
  EXPECT_EQ(t.qfi, 5);
  EXPECT_EQ(t.tx_gpdupkts, 9U);
  EXPECT_FLOAT_EQ(t.tx_loss_rate, 0.1F);
  EXPECT_EQ(t.rx_bytes, 400U);
  EXPECT_EQ(t.rx_unkwn_teid_pkts, 1U);
  EXPECT_EQ(t.tx_proc_latency_us, 2U);
}

TEST_F(aurora_core_test, handover_counters_per_du_and_totals)
{
  const uint64_t du_a = 0x1'0000'0011ULL; // gNB-DU IDs are up to 36 bits.
  const uint64_t du_b = 22;
  on_handover_attempt(du_a);
  on_handover_attempt(du_a);
  on_handover_success(du_a);
  on_handover_attempt(du_b);
  on_handover_success(du_b);
  on_handover_preparation_attempt();
  on_handover_preparation_success();
  on_du_count(3);
  on_serving_cell_rsrq(-10.0F);
  on_serving_cell_rsrq(-12.0F);
  const aurora_metrics& r = publish();
  // RC cell 0: totals over DUs, cumulative.
  EXPECT_EQ(r.rc_cell_count, 1U);
  EXPECT_EQ(r.rc_metrics[0].handover_attempts, 3U);
  EXPECT_EQ(r.rc_metrics[0].handover_success, 2U);
  EXPECT_NEAR(r.rc_metrics[0].handover_success_rate, 2.0 / 3, 1e-9);
  EXPECT_DOUBLE_EQ(r.rc_metrics[0].rsrq_avg, -11.0);
  EXPECT_EQ(r.du_count, 3U);
  // Per-DU table.
  ASSERT_EQ(r.ext.ho_du_count, 2U);
  EXPECT_EQ(r.ext.ho_du[0].gnb_du_id, du_a);
  EXPECT_EQ(r.ext.ho_du[0].exec_attempts, 2U);
  EXPECT_EQ(r.ext.ho_du[0].exec_success, 1U);
  EXPECT_DOUBLE_EQ(r.ext.ho_du[0].exec_success_rate, 0.5);
  EXPECT_EQ(r.ext.ho_du[1].gnb_du_id, du_b);
  EXPECT_DOUBLE_EQ(r.ext.ho_du[1].exec_success_rate, 1.0);
  EXPECT_EQ(r.ext.ho_prep_attempts, 1U);
  EXPECT_EQ(r.ext.ho_prep_success_total, 1U);
  // Next interval: deltas reset, totals kept.
  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.ext.ho_du[0].exec_attempts, 0U);
  EXPECT_EQ(r2.ext.ho_du[0].exec_attempts_total, 2U);
  EXPECT_EQ(r2.ext.ho_prep_attempts, 0U);
  EXPECT_EQ(r2.rc_metrics[0].handover_attempts, 3U);
}

TEST_F(aurora_core_test, full_csi_report_is_published_in_extension)
{
  on_ue_created(8, 0x4608);
  csi_info c;
  c.nof_rs         = 2;
  c.cri            = {3, 1, 0, 0};
  c.l1_rsrp_dbm    = {-95, -80, 0, 0};
  c.rsrp_from_ssb  = true;
  c.ri             = 2;
  c.li             = 1;
  c.wb_cqi         = 12;
  c.wb_cqi_tb2     = 9;
  c.nof_subbands_tb1 = 3;
  c.sb_cqi_diff_tb1  = {0, 1, 3};
  c.pmi            = csi_info::pmi_kind::typeI_single_panel;
  c.pmi_i1         = 5;
  c.pmi_i12        = 2;
  c.pmi_i2         = 1;
  on_csi_report(8, c);
  csi_info bad;
  bad.valid = false;
  on_csi_report(8, bad);

  const aurora_metrics&        r = publish();
  const aurora_csi_ue_stats_t& x = r.ext.csi[8];
  EXPECT_EQ(r.ext.version, unsigned(AURORA_EXT_VERSION));
  EXPECT_EQ(x.rnti, 0x4608U);
  EXPECT_EQ(x.nof_reports, 2U);
  EXPECT_EQ(x.nof_invalid_reports, 1U);
  EXPECT_EQ(x.nof_rs, 2);
  EXPECT_EQ(x.cri[0], 3);
  EXPECT_EQ(x.l1_rsrp_dbm[1], -80);
  EXPECT_EQ(x.l1_rsrp_best_dbm, -80);
  EXPECT_EQ(x.rsrp_from_ssb, 1);
  EXPECT_EQ(x.ri, 2);
  EXPECT_EQ(x.li, 1);
  EXPECT_EQ(x.wb_cqi_tb1, 12);
  EXPECT_EQ(x.wb_cqi_tb2, 9);
  EXPECT_EQ(x.nof_subbands_tb1, 3);
  EXPECT_EQ(x.sb_cqi_diff_tb1[2], 3);
  EXPECT_EQ(x.nof_subbands_tb2, 0);
  EXPECT_EQ(x.pmi_type, AURORA_PMI_TYPEI_SP);
  EXPECT_EQ(x.pmi_i11, 5);
  EXPECT_EQ(x.pmi_i12, 2);
  EXPECT_EQ(x.pmi_i13, AURORA_CSI_NOT_REPORTED);
  EXPECT_EQ(x.pmi_i2, 1);
  // OAI-layout mirrors.
  EXPECT_EQ(r.mac_ue_stats[8].ssb_rsrp_best, -80);
  EXPECT_EQ(r.mac_ue_stats[8].ri, 2);
  EXPECT_EQ(r.mac_ue_stats[8].pmi_x1, 5);
  // Counters are per interval; the last report persists.
  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.ext.csi[8].nof_reports, 0U);
  EXPECT_EQ(r2.ext.csi[8].ri, 2);
}

TEST_F(aurora_core_test, csi_quantities_are_not_reported_until_first_report)
{
  on_ue_created(13, 0x4613);
  const aurora_csi_ue_stats_t& x = publish().ext.csi[13];
  EXPECT_EQ(x.rnti, 0x4613U);
  EXPECT_EQ(x.ri, AURORA_CSI_NOT_REPORTED);
  EXPECT_EQ(x.wb_cqi_tb1, AURORA_CSI_NOT_REPORTED);
  EXPECT_EQ(x.pmi_i11, AURORA_CSI_NOT_REPORTED);
  EXPECT_EQ(x.pmi_type, AURORA_PMI_NONE);
  EXPECT_EQ(x.nof_rs, 0);
}

TEST_F(aurora_core_test, type2_pmi_coefficients_are_published)
{
  on_ue_created(9, 0x4609);
  csi_info c;
  c.pmi              = csi_info::pmi_kind::typeII;
  c.pmi_i1           = 4;
  c.t2_i12           = 300;
  c.t2_nof_layers    = 2;
  c.t2_i13           = {6, 2};
  c.t2_nof_coeffs    = {8, 8};
  c.t2_i14[1][7]     = 7;
  c.t2_i21[0][3]     = 5;
  c.t2_i22[1][0]     = 1;
  on_csi_report(9, c);
  const aurora_csi_ue_stats_t& x = publish().ext.csi[9];
  EXPECT_EQ(x.pmi_type, AURORA_PMI_TYPEII);
  EXPECT_EQ(x.pmi_i11, 4);
  EXPECT_EQ(x.t2_i12, 300);
  EXPECT_EQ(x.t2_nof_layers, 2);
  EXPECT_EQ(x.t2_i13[1], 2);
  EXPECT_EQ(x.t2_i14[1][7], 7);
  EXPECT_EQ(x.t2_i21[0][3], 5);
  EXPECT_EQ(x.t2_i22[1][0], 1);
}

TEST_F(aurora_core_test, ul_failure_timer_counts_milliseconds_since_rlf)
{
  on_ue_created(12, 0x4612);
  on_ul_failure(12, true);
  std::this_thread::sleep_for(std::chrono::milliseconds{120});
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.mac_ue_stats[12].ul_failure, 1);
  EXPECT_GE(r.mac_ue_stats[12].ul_failure_timer, 120U);
  EXPECT_LT(r.mac_ue_stats[12].ul_failure_timer, 2000U);
  on_ul_failure(12, false);
  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.mac_ue_stats[12].ul_failure, 0);
  EXPECT_EQ(r2.mac_ue_stats[12].ul_failure_timer, 0U);
}

TEST_F(aurora_core_test, events_of_unknown_or_removed_ues_do_not_create_ghost_slots)
{
  // Never created: global counters move, no per-UE slot appears.
  on_pdsch_grant(make_grant(10, 0x4610, 5, 5, 50));
  on_ul_crc(10, 0x4610, true, 3.0F);
  rlc_add(10, drb_rb_index(1), rlc_counter::txsdu_pkts, 1);
  pdcp_add(10, drb_rb_index(1), pdcp_counter::txsdu_pkts, 1);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.fapi_dl_total_pdsch_count, 1U);
  EXPECT_EQ(r.ul_crc_total, 1U);
  EXPECT_EQ(r.mac_ue_stats[10].rnti, 0U);
  EXPECT_EQ(r.rlc_rb_stats[10][4].rnti, 0U);
  EXPECT_EQ(r.pdcp_rb_stats[10][4].rnti, 0U);
  EXPECT_EQ(r.active_ue_count, 0U);

  // Removed: late events do not resurrect the slot.
  on_ue_created(11, 0x4611);
  rlc_bearer_created(11, drb_rb_index(1), rlc_mode::am);
  on_ue_removed(11);
  on_pdsch_grant(make_grant(11, 0x4611, 5, 5, 50));
  rlc_add(11, drb_rb_index(1), rlc_counter::txsdu_pkts, 1);
  const aurora_metrics& r2 = publish();
  EXPECT_EQ(r2.mac_ue_stats[11].rnti, 0U);
  EXPECT_EQ(r2.rlc_rb_stats[11][4].rnti, 0U);
}

TEST_F(aurora_core_test, aliased_ue_indexes_do_not_clobber_each_other)
{
  // UE 5 and UE 105 share slot 5; only the owner may update or free it.
  on_ue_created(5, 0x4605);
  on_ue_removed(105);
  on_pdsch_grant(make_grant(105, 0x4699, 5, 5, 50));
  on_pdsch_grant(make_grant(5, 0x4605, 7, 7, 70));
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.mac_ue_stats[5].rnti, 0x4605U);
  EXPECT_EQ(r.mac_ue_stats[5].dl_aggr_tbs, 70U);
}

TEST_F(aurora_core_test, ue_creation_resets_leftovers_of_previous_slot_owner)
{
  on_ue_created(6, 0x4606);
  on_pdsch_grant(make_grant(6, 0x4606, 5, 5, 500));
  on_ue_removed(6);
  on_ue_created(6, 0x4607);
  const aurora_metrics& r = publish();
  EXPECT_EQ(r.mac_ue_stats[6].rnti, 0x4607U);
  EXPECT_EQ(r.mac_ue_stats[6].dl_aggr_tbs, 0U);
}

TEST_F(aurora_core_test, gtp_tunnel_removal_by_another_ue_is_ignored)
{
  ASSERT_EQ(gtp_tunnel_created(3, 0x10, 0x20), 0);
  gtp_tunnel_removed(103, 0);
  gtp_add(3, 0, gtp_counter::rx_pkts, 2);
  EXPECT_EQ(publish().gtp_tunnel_stats[3][0].rx_pkts, 2U);
}

TEST_F(aurora_core_test, rusage_and_timestamp)
{
  uint64_t t0 = m->timestamp_us;
  const aurora_metrics& r = publish();
  EXPECT_GT(r.ru_maxrss_kb, 0U);
  EXPECT_GE(r.timestamp_us, t0);
}

extern "C" void aurora_thread_registry_init(const aurora_thread_config_t* cfg);

namespace {

void load_whitelist(const char* list)
{
  aurora_thread_config_t cfg{};
  cfg.enabled = 1;
  std::snprintf(cfg.whitelist, sizeof(cfg.whitelist), "%s", list);
  aurora_thread_registry_init(&cfg);
}

} // namespace

TEST(aurora_thread_whitelist, default_list_fits_limits_and_matches_ocudu_thread_names)
{
  const std::string list = default_thread_whitelist();
  ASSERT_LT(list.size(), size_t(AURORA_THREAD_WHITELIST_MAX)) << "whitelist would be truncated";
  unsigned nof_patterns = 1;
  for (char c : list) {
    nof_patterns += c == ',';
  }
  ASSERT_LE(nof_patterns, 64U) << "registry keeps at most 64 patterns";

  load_whitelist(list.c_str());
  for (const char* name : {"gnb",          "odu",           "ocu",           "ocucp",         "ocuup",
                           "odu_low",      "main_pool#0",   "main_pool#15",  "phy_worker",    "radio",
                           "split6",       "ru_timing",     "ru_txrx_#0",    "ru_dummy",      "lower_phy#0",
                           "lower_phy_tx#1", "lower_phy_rx#0", "lower_phy_ul#0", "io_broker_epoll", "ws_server",
                           "tracer_worker", "rx_symb_print", "SRSIQDump",     "ULPhyTap",      "ZMQbg/Reaper",
                           "ZMQbg/IO/0",   "aurora_reset",  "aurora_thr_proc", "aurora_thr_ebpf", "async_thread",
                           "lower_phy_prach", "low_rx",     "ru_rx_#0",      "ru_emu_#3",     "radio_util_samp",
                           "rx_power_analyz"}) {
    EXPECT_TRUE(aurora_thread_name_matches(name)) << name;
  }
  for (const char* name : {"main_pool", "phy_worker#0", "foo", "gnb_extra", "lower_phy_x"}) {
    EXPECT_FALSE(aurora_thread_name_matches(name)) << name;
  }
}

TEST(aurora_thread_whitelist, multi_line_yaml_whitespace_is_trimmed)
{
  load_whitelist("  main_pool#* ,\n    radio\t,\r\n ws_server\n");
  EXPECT_TRUE(aurora_thread_name_matches("main_pool#2"));
  EXPECT_TRUE(aurora_thread_name_matches("radio"));
  EXPECT_TRUE(aurora_thread_name_matches("ws_server"));
  EXPECT_FALSE(aurora_thread_name_matches("radio2"));
}
