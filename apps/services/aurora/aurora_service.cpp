#include "aurora_service.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <cstdlib>

using namespace ocudu;
using namespace app_services;

static aurora::ran_type parse_ran_type(const std::string& s, aurora::ran_type default_type)
{
  if (s == "gnb") {
    return aurora::ran_type::gnb;
  }
  if (s == "du") {
    return aurora::ran_type::du;
  }
  if (s == "cu") {
    return aurora::ran_type::cu;
  }
  if (s == "cu_cp") {
    return aurora::ran_type::cu_cp;
  }
  if (s == "cu_up") {
    return aurora::ran_type::cu_up;
  }
  return default_type;
}

static aurora::thread_backend parse_backend(const std::string& s)
{
  if (s == "ebpf") {
    return aurora::thread_backend::ebpf;
  }
  if (s == "procfs") {
    return aurora::thread_backend::procfs;
  }
  return aurora::thread_backend::automatic;
}

aurora::config app_services::to_aurora_config(const aurora_appconfig& cfg, aurora::ran_type default_type)
{
  aurora::config out;
  out.enabled                  = cfg.enabled;
  out.type                     = parse_ran_type(cfg.ran_type, default_type);
  out.collection_interval      = cfg.collection_interval;
  out.shm_key                  = static_cast<uint32_t>(std::strtoul(cfg.shm_key.c_str(), nullptr, 0));
  out.thread_metrics_enabled   = cfg.thread_metrics_enabled;
  out.thread_metrics_backend   = parse_backend(cfg.thread_metrics_backend);
  out.thread_metrics_whitelist = cfg.thread_metrics_whitelist;
  out.anomaly_enabled          = cfg.anomaly_enabled;
  return out;
}

aurora_service::aurora_service(const aurora_appconfig& cfg, aurora::ran_type default_type)
{
  if (!cfg.enabled) {
    return;
  }
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("ALL");
  if (aurora::start(to_aurora_config(cfg, default_type)) != 0) {
    logger.error("AURORA: failed to start (shm_key={})", cfg.shm_key);
    return;
  }
  started = true;
  logger.info("AURORA: started shm_key={} interval={}ms", cfg.shm_key, cfg.collection_interval.count());
}

aurora_service::~aurora_service()
{
  if (started) {
    aurora::stop();
  }
}
