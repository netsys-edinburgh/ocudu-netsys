#pragma once

#include "CLI/CLI11.hpp"

namespace ocudu {
namespace app_services {

struct aurora_appconfig;

/// Registers the "aurora" configuration section.
void configure_cli11_with_aurora_config_schema(CLI::App& app, aurora_appconfig& config);

} // namespace app_services
} // namespace ocudu
