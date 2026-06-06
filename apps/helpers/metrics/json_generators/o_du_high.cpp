// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "o_du_high.h"
#include "apps/helpers/metrics/helpers.h"
#include "du_high/mac.h"
#include "ocudu/du/du_high/du_metrics_report.h"

using namespace ocudu;
using namespace app_helpers;
using namespace json_generators;

nlohmann::json ocudu::app_helpers::json_generators::generate(const odu::du_metrics_report& metrics)
{
  nlohmann::json json;

  json["timestamp"]  = get_time_stamp();
  auto& json_du      = json["du"];
  auto& json_du_high = json_du["du_high"];

  if (auto dl_json = generate(metrics.mac->dl); !dl_json.is_null()) {
    json_du_high["mac"].update(dl_json);
  }
  if (auto ul_json = generate(metrics.mac->ul); !ul_json.is_null()) {
    json_du_high["mac"].update(ul_json);
  }

  return json;
}

std::string ocudu::app_helpers::json_generators::generate_string(const odu::du_metrics_report& metrics, int indent)
{
  return generate(metrics).dump(indent);
}
