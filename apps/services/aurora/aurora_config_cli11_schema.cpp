#include "aurora_config_cli11_schema.h"
#include "aurora_appconfig.h"
#include "apps/services/worker_manager/cli11_cpu_affinities_parser_helper.h"
#include "ocudu/support/cli11_utils.h"

using namespace ocudu;
using namespace app_services;

void ocudu::app_services::configure_cli11_with_aurora_config_schema(CLI::App& app, aurora_appconfig& config)
{
  CLI::App* sub = add_subcommand(app, "aurora", "AURORA shared-memory RAN metrics framework")->configurable();
  add_option(*sub, "--enable", config.enabled, "Enable AURORA metrics collection")->capture_default_str();
  add_option(*sub, "--collection_interval_ms", config.collection_interval, "Shared memory publication interval")
      ->capture_default_str()
      ->check(CLI::Range(1, 60000));
  add_option(*sub, "--shm_key", config.shm_key, "SysV shared memory key (hex, e.g. 0xDEADBEEF)")
      ->capture_default_str()
      ->check([](const std::string& value) -> std::string {
        char*         end = nullptr;
        unsigned long key = std::strtoul(value.c_str(), &end, 0);
        if (value.empty() || end == nullptr || *end != '\0' || key == 0 || key > 0xffffffffUL) {
          return "Invalid SysV key '" + value + "'";
        }
        return {};
      });
  add_option(*sub, "--ran_type", config.ran_type, "RAN node type reported in shared memory")
      ->capture_default_str()
      ->check(CLI::IsMember({"auto", "gnb", "du", "cu", "cu_cp", "cu_up"}));
  add_option(*sub, "--thread_metrics_enabled", config.thread_metrics_enabled, "Enable per-thread runtime metrics")
      ->capture_default_str();
  add_option(*sub, "--thread_metrics_backend", config.thread_metrics_backend, "Thread metrics backend")
      ->capture_default_str()
      ->check(CLI::IsMember({"auto", "ebpf", "procfs"}));
  add_option(*sub,
             "--thread_metrics_whitelist",
             config.thread_metrics_whitelist,
             "Comma-separated thread names to monitor; trailing '*' = prefix match. Empty = OCUDU default list")
      ->check(CLI::Validator(
          [](const std::string& value) -> std::string {
            return value.size() < 512 ? std::string{} : std::string("Whitelist longer than 511 characters");
          },
          "<512 chars"));
  add_option(*sub, "--anomaly_enabled", config.anomaly_enabled, "Enable the anomaly-injection TCP control server")
      ->capture_default_str();
  add_option_function<std::string>(
      *sub,
      "--cpus",
      [&config](const std::string& value) { parse_affinity_mask(config.cpus, value, "cpus"); },
      "CPUs of the AURORA publisher thread. Default: the main thread pool CPUs");
}
