#include "aurora_sched_tap.h"
#include "../config/cell_configuration.h"
#include "ocudu/aurora/aurora.h"
#include "ocudu/ran/csi_report/csi_report_data.h"
#include "ocudu/ran/csi_rs/csi_meas_config.h"
#include "ocudu/ran/resource_allocation/rb_bitmap.h"
#include "ocudu/scheduler/result/sched_result.h"

using namespace ocudu;

namespace {

template <typename RbAlloc>
unsigned grant_nof_prbs(const RbAlloc& rbs, unsigned nof_bwp_prbs)
{
  if (rbs.is_type0()) {
    return convert_rbgs_to_prbs(rbs.type0(), {0, nof_bwp_prbs}, get_nominal_rbg_size(nof_bwp_prbs, true)).count();
  }
  return rbs.type1().length();
}

} // namespace

void ocudu::aurora_report_slot_result(const cell_configuration&  cell_cfg,
                                      slot_point                 sl_tx,
                                      const sched_result&        result,
                                      std::chrono::microseconds decision_latency)
{
  if (not aurora::is_active()) {
    return;
  }

  for (const dl_msg_alloc& grant : result.dl.ue_grants) {
    if (grant.pdsch_cfg.codewords.empty()) {
      continue;
    }
    aurora::grant_info g;
    g.ue_index   = grant.context.ue_index;
    g.rnti       = to_underlying(grant.pdsch_cfg.rnti);
    g.mcs        = grant.pdsch_cfg.codewords[0].mcs_index.value();
    g.mcs_table  = static_cast<unsigned>(grant.pdsch_cfg.mcs_table);
    g.nof_prbs   = grant_nof_prbs(grant.pdsch_cfg.rbs, cell_cfg.nof_dl_prbs);
    g.tbs_bytes  = 0;
    for (const auto& cw : grant.pdsch_cfg.codewords) {
      g.tbs_bytes += cw.tb_size_bytes.value();
    }
    g.nof_retxs  = grant.context.nof_retxs;
    g.sfn        = sl_tx.sfn();
    g.slot_index = sl_tx.slot_index();
    aurora::on_pdsch_grant(g);
  }

  for (const ul_sched_info& grant : result.ul.puschs) {
    aurora::grant_info g;
    g.ue_index   = grant.context.ue_index;
    g.rnti       = to_underlying(grant.pusch_cfg.rnti);
    g.mcs        = grant.pusch_cfg.mcs_index.value();
    g.mcs_table  = static_cast<unsigned>(grant.pusch_cfg.mcs_table);
    g.nof_prbs   = grant_nof_prbs(grant.pusch_cfg.rbs, cell_cfg.nof_ul_prbs);
    g.tbs_bytes  = grant.pusch_cfg.tb_size_bytes.value();
    g.nof_retxs  = grant.context.nof_retxs;
    g.sfn        = sl_tx.sfn();
    g.slot_index = sl_tx.slot_index();
    aurora::on_pusch_grant(g);
  }

  aurora::on_scheduler_slot(static_cast<unsigned>(cell_cfg.params.pci),
                            decision_latency,
                            cell_cfg.nof_dl_prbs,
                            result.dl.nof_dl_symbols > 0,
                            result.ul.nof_ul_symbols > 0);
}

void ocudu::aurora_report_csi(unsigned ue_index, const csi_meas_config* csi_meas_cfg, const csi_report_data& csi)
{
  if (not aurora::is_active() or not csi.valid) {
    return;
  }
  aurora::csi_info info;
  if (csi.first_tb_wideband_cqi.has_value()) {
    info.wb_cqi = csi.first_tb_wideband_cqi->value();
  }
  if (csi.ri.has_value()) {
    info.ri = csi.ri->value();
  }
  if (csi.pmi.has_value()) {
    // OAI pmi_x1/pmi_x2 = wideband i1/i2 (Type-I single panel); the 2-port codebook only has i2.
    if (const auto* two = std::get_if<pmi_two_antenna_port>(&*csi.pmi)) {
      info.pmi_i1 = 0;
      info.pmi_i2 = two->pmi;
    } else if (const auto* t1 = std::get_if<pmi_typeI_single_panel>(&*csi.pmi)) {
      info.pmi_i1 = t1->i_1_1;
      info.pmi_i2 = t1->i_2;
    } else if (const auto* t2 = std::get_if<pmi_typeII>(&*csi.pmi)) {
      info.pmi_i1 = t2->i_1_1;
    }
  }
  if (not csi.rsrp_dBm.empty()) {
    // Strongest reported resource.
    int16_t best = csi.rsrp_dBm[0];
    for (int16_t v : csi.rsrp_dBm) {
      best = std::max(best, v);
    }
    info.l1_rsrp_dbm = static_cast<float>(best);
    info.rsrp_from_ssb = false;
    if (csi_meas_cfg != nullptr) {
      for (const auto& rep : csi_meas_cfg->csi_report_cfg_list) {
        if (rep.report_qty_type == csi_report_config::report_quantity_type_t::ssb_index_rsrp) {
          info.rsrp_from_ssb = true;
        }
      }
    }
  }
  aurora::on_csi_report(ue_index, info);
}
