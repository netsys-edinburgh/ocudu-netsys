#pragma once

#include <yaml-cpp/yaml.h>

namespace ocudu {
namespace app_services {

struct aurora_appconfig;

/// Fills the "aurora" section of the given YAML node.
void fill_aurora_config_in_yaml_schema(YAML::Node& node, const aurora_appconfig& config);

} // namespace app_services
} // namespace ocudu
