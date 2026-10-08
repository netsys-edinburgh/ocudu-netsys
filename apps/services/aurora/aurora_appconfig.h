#pragma once

#include <chrono>
#include <string>

namespace ocudu {
namespace app_services {

/// AURORA metrics framework application configuration (YAML section "aurora").
struct aurora_appconfig {
  /// Enables the AURORA shared-memory metrics framework.
  bool enabled = false;
  /// Interval at which the shared memory snapshot is published.
  std::chrono::milliseconds collection_interval{100};
  /// SysV shared memory key, hexadecimal string (e.g. "0xDEADBEEF"). Must be unique per process on a host.
  std::string shm_key = "0xDEADBEEF";
  /// RAN node type reported to readers: auto (derived from the application), gnb, du, cu, cu_cp or cu_up.
  std::string ran_type = "auto";
  /// Per-thread runtime distribution metrics.
  bool thread_metrics_enabled = true;
  /// Thread metrics backend: auto (eBPF, falling back to procfs), ebpf or procfs.
  std::string thread_metrics_backend = "auto";
  /// Comma-separated thread names to monitor ('*' suffix = prefix match). Empty selects the OCUDU default list.
  std::string thread_metrics_whitelist;
  /// Anomaly-injection TCP control server on 127.0.0.1:(47010 + ran_type).
  bool anomaly_enabled = true;
};

} // namespace app_services
} // namespace ocudu
