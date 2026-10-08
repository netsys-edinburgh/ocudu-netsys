#pragma once

#include "aurora_appconfig.h"
#include "ocudu/aurora/aurora.h"

namespace ocudu {
namespace app_services {

/// Starts AURORA on construction (when enabled in the configuration) and stops it on destruction.
class aurora_service
{
public:
  /// \param cfg          Application "aurora" section.
  /// \param default_type RAN type of the application, used when cfg.ran_type is "auto".
  aurora_service(const aurora_appconfig& cfg, aurora::ran_type default_type);
  ~aurora_service();
  aurora_service(const aurora_service&)            = delete;
  aurora_service& operator=(const aurora_service&) = delete;

private:
  bool started = false;
};

/// Translates the application configuration into the AURORA library configuration.
aurora::config to_aurora_config(const aurora_appconfig& cfg, aurora::ran_type default_type);

} // namespace app_services
} // namespace ocudu
