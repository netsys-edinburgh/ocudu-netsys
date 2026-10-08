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
  if (not aurora::is_active()) {
    return;
  }
  aurora::csi_info info;
  info.valid = csi.valid;
  if (not csi.valid) {
    aurora::on_csi_report(ue_index, info);
    return;
  }

  // CRI / SSBRI and L1-RSRP.
  info.nof_rs = std::min<unsigned>(csi.rsrp_dBm.size(), aurora::csi_info::max_rs);
  for (unsigned i = 0; i != info.nof_rs; ++i) {
    info.l1_rsrp_dbm[i] = csi.rsrp_dBm[i];
    info.cri[i]         = i < csi.cri.size() ? csi.cri[i] : 0;
  }
  if (csi_meas_cfg != nullptr) {
    for (const auto& rep : csi_meas_cfg->csi_report_cfg_list) {
      if (rep.report_qty_type == csi_report_config::report_quantity_type_t::ssb_index_rsrp) {
        info.rsrp_from_ssb = true;
      }
    }
  }

  // RI / LI / CQI.
  if (csi.ri.has_value()) {
    info.ri = csi.ri->value();
  }
  if (csi.li.has_value()) {
    info.li = csi.li->value();
  }
  if (csi.first_tb_wideband_cqi.has_value()) {
    info.wb_cqi = csi.first_tb_wideband_cqi->value();
  }
  if (csi.second_tb_wideband_cqi.has_value()) {
    info.wb_cqi_tb2 = csi.second_tb_wideband_cqi->value();
  }
  if (csi.first_tb_subband_diff_cqi.has_value()) {
    info.nof_subbands_tb1 = std::min<unsigned>(csi.first_tb_subband_diff_cqi->size(), aurora::csi_info::max_subbands);
    for (unsigned i = 0; i != info.nof_subbands_tb1; ++i) {
      info.sb_cqi_diff_tb1[i] = (*csi.first_tb_subband_diff_cqi)[i].value();
    }
  }
  if (csi.second_tb_subband_diff_cqi.has_value()) {
    info.nof_subbands_tb2 = std::min<unsigned>(csi.second_tb_subband_diff_cqi->size(), aurora::csi_info::max_subbands);
    for (unsigned i = 0; i != info.nof_subbands_tb2; ++i) {
      info.sb_cqi_diff_tb2[i] = (*csi.second_tb_subband_diff_cqi)[i].value();
    }
  }

  // PMI. OAI pmi_x1/pmi_x2 = wideband i1/i2 (Type-I single panel); the two-port codebook only has i2.
  if (csi.pmi.has_value()) {
    if (const auto* two = std::get_if<pmi_two_antenna_port>(&*csi.pmi)) {
      info.pmi          = aurora::csi_info::pmi_kind::two_port;
      info.pmi_two_port = two->pmi;
      info.pmi_i1       = 0;
      info.pmi_i2       = two->pmi;
    } else if (const auto* t1 = std::get_if<pmi_typeI_single_panel>(&*csi.pmi)) {
      info.pmi    = aurora::csi_info::pmi_kind::typeI_single_panel;
      info.pmi_i1 = t1->i_1_1;
      if (t1->i_1_2.has_value()) {
        info.pmi_i12 = *t1->i_1_2;
      }
      if (t1->i_1_3.has_value()) {
        info.pmi_i13 = *t1->i_1_3;
      }
      info.pmi_i2 = t1->i_2;
    } else if (const auto* t2 = std::get_if<pmi_typeII>(&*csi.pmi)) {
      info.pmi           = aurora::csi_info::pmi_kind::typeII;
      info.pmi_i1        = t2->i_1_1;
      info.t2_i12        = t2->i_1_2;
      info.t2_nof_layers = std::min<unsigned>(t2->layers.size(), aurora::csi_info::max_t2_layers);
      for (unsigned l = 0; l != info.t2_nof_layers; ++l) {
        const auto& layer = t2->layers[l];
        info.t2_i13[l]    = layer.i_1_3;
        unsigned n        = std::min<unsigned>(layer.i_1_4.size(), aurora::csi_info::max_t2_coeffs);
        info.t2_nof_coeffs[l] = n;
        for (unsigned k = 0; k != n; ++k) {
          info.t2_i14[l][k] = layer.i_1_4[k];
          info.t2_i21[l][k] = k < layer.i_2_1.size() ? layer.i_2_1[k] : 0;
          info.t2_i22[l][k] = k < layer.i_2_2.size() ? layer.i_2_2[k] : 0;
        }
      }
    }
  }
  aurora::on_csi_report(ue_index, info);
}
