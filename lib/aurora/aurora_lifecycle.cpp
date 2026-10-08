// AURORA lifecycle: shared memory segment, publisher thread ("aurora_reset"), thread-metrics collector and
// anomaly-injection server (port of OAI aurora_init()/aurora_get_metrics()/aurora_cleanup()).

#include "anomaly/anomaly_module.h"
#include "aurora_state.h"
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/shm.h>
#include <thread>

using namespace ocudu;
using namespace aurora;

extern "C" void aurora_thread_set_interval_ms(uint32_t interval_ms);

std::atomic<bool>     aurora::detail::active{false};
detail::global_state* aurora::detail::state = nullptr;
aurora_metrics*       aurora::detail::shm   = nullptr;

namespace {

std::unique_ptr<detail::global_state> owned_state;
int                                   shm_id = -1;
std::thread                           publisher;
std::mutex                            stop_mutex;
std::condition_variable               stop_cv;
bool                                  stop_requested = false;
bool                                  thread_metrics_started = false;

void publisher_loop(std::chrono::milliseconds period)
{
  ::pthread_setname_np(::pthread_self(), "aurora_reset");
  auto next = std::chrono::steady_clock::now() + period;
  std::unique_lock<std::mutex> lock(stop_mutex);
  while (!stop_cv.wait_until(lock, next, [] { return stop_requested; })) {
    lock.unlock();
    detail::publish_interval(*detail::shm, *detail::state);
    lock.lock();
    next += period;
    // Do not try to catch up after a stall (e.g. the process was suspended).
    auto now = std::chrono::steady_clock::now();
    if (next < now) {
      next = now + period;
    }
  }
}

aurora_metrics* create_segment(key_t key)
{
  shm_id = ::shmget(key, sizeof(aurora_metrics), IPC_CREAT | 0666);
  if (shm_id < 0 && errno == EINVAL) {
    // A stale segment with another size (e.g. from an older build) exists: remove it and retry.
    int old = ::shmget(key, 0, 0666);
    if (old >= 0) {
      ::shmctl(old, IPC_RMID, nullptr);
    }
    shm_id = ::shmget(key, sizeof(aurora_metrics), IPC_CREAT | 0666);
  }
  if (shm_id < 0) {
    std::fprintf(stderr, "AURORA: shmget(key=0x%x) failed: %s\n", unsigned(key), std::strerror(errno));
    return nullptr;
  }
  void* p = ::shmat(shm_id, nullptr, 0);
  if (p == reinterpret_cast<void*>(-1)) {
    std::fprintf(stderr, "AURORA: shmat failed: %s\n", std::strerror(errno));
    return nullptr;
  }
  return static_cast<aurora_metrics*>(p);
}

} // namespace

const char* aurora::default_thread_whitelist()
{
  // Every thread name created by the OCUDU code base (names as seen in /proc/<pid>/task/<tid>/comm, i.e. truncated
  // to 15 characters by unique_thread). '#*' entries are worker pools / per-cell / per-sector threads.
  return
      // Main thread of each application; the ocudulog backend thread inherits it.
      "gnb,odu,ocu,ocucp,ocuup,odu_low,"
      // apps/services/worker_manager.
      "main_pool#*,phy_worker,radio,split6,ru_timing,ru_txrx_#*,ru_dummy,"
      "lower_phy#*,lower_phy_tx#*,lower_phy_rx#*,lower_phy_ul#*,"
      // Support threads: IO broker, remote control server, event tracer, PHY RX grid printer.
      "io_broker_epoll,ws_server,tracer_worker,rx_symb_print,"
      // ocudu_phy_tap external processors and the libzmq background threads used by the ZMQ radio / PHY tap.
      "SRSIQDump,ULPhyTap,ZMQbg/*,"
      // AURORA itself.
      "aurora_reset,aurora_thr_proc,aurora_thr_ebpf,"
      // apps/examples (radio_ssb, radio_util_sample, rx_power_analyzer, ru_emulator).
      "async_thread,lower_phy,lower_phy_dl,lower_phy_ul,lower_phy_prach,low_rx,low_tx,low_dl,low_ul,"
      "ru_rx_#*,ru_emu_#*,ru_emulator,radio_ssb,radio_util_samp,rx_power_analyz";
}

int aurora::start(const config& cfg)
{
  if (!cfg.enabled) {
    return 0;
  }
  if (detail::active.load()) {
    return 0;
  }
  if (cfg.collection_interval.count() <= 0) {
    std::fprintf(stderr, "AURORA: invalid collection interval\n");
    return -1;
  }

  owned_state      = std::make_unique<detail::global_state>();
  owned_state->cfg = cfg;
  if (owned_state->cfg.thread_metrics_whitelist.empty()) {
    owned_state->cfg.thread_metrics_whitelist = default_thread_whitelist();
  }

  aurora_metrics* m = create_segment(static_cast<key_t>(cfg.shm_key));
  if (m == nullptr) {
    owned_state.reset();
    return -1;
  }
  detail::init_shm_content(*m, owned_state->cfg);
  detail::shm   = m;
  detail::state = owned_state.get();

  if (owned_state->cfg.thread_metrics_enabled) {
    aurora_thread_config_t tcfg{};
    tcfg.enabled = 1;
    tcfg.backend = static_cast<aurora_thread_backend_t>(owned_state->cfg.thread_metrics_backend);
    std::snprintf(tcfg.whitelist, sizeof(tcfg.whitelist), "%s", owned_state->cfg.thread_metrics_whitelist.c_str());
    aurora_thread_set_interval_ms(uint32_t(cfg.collection_interval.count()));
    if (aurora_thread_init(&tcfg, &m->thread_stats) == 0) {
      thread_metrics_started = true;
    } else {
      std::fprintf(stderr, "AURORA: thread metrics collector failed to start, continuing without it\n");
      owned_state->cfg.thread_metrics_enabled = false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(stop_mutex);
    stop_requested = false;
  }
  detail::active.store(true);
  publisher = std::thread(publisher_loop, cfg.collection_interval);

  if (cfg.anomaly_enabled) {
    anomaly_module_config_t acfg{};
    acfg.ran_type     = static_cast<int>(cfg.type);
    acfg.listen_port  = anomaly_module_default_port_for_ran(acfg.ran_type);
    acfg.bind_address = "127.0.0.1";
    if (anomaly_module_start(&acfg) != 0) {
      std::fprintf(stderr, "AURORA: anomaly module failed to start for role=%s\n", anomaly_module_role_name(acfg.ran_type));
    }
  }

  std::fprintf(stdout,
               "AURORA: started key=0x%x ran_type=%d interval=%lldms size=%zu bytes\n",
               cfg.shm_key,
               static_cast<int>(cfg.type),
               static_cast<long long>(cfg.collection_interval.count()),
               sizeof(aurora_metrics));
  return 0;
}

void aurora::stop()
{
  if (!detail::active.exchange(false)) {
    return;
  }
  anomaly_module_stop();
  {
    std::lock_guard<std::mutex> lock(stop_mutex);
    stop_requested = true;
  }
  stop_cv.notify_all();
  if (publisher.joinable()) {
    publisher.join();
  }
  if (thread_metrics_started) {
    aurora_thread_shutdown();
    thread_metrics_started = false;
  }
  if (detail::shm != nullptr) {
    ::shmdt(detail::shm);
    detail::shm = nullptr;
  }
  if (shm_id >= 0) {
    ::shmctl(shm_id, IPC_RMID, nullptr);
    shm_id = -1;
  }
  // Producers may still be inside a hook that loaded `active == true`; keep the state alive until process exit.
  detail::state = nullptr;
  (void)owned_state.release();
}
