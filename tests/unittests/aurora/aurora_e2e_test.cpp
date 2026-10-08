// End-to-end test: real publisher thread and procfs thread-metrics collector, observed through the SysV segment
// exactly as aurora_reader sees it.

#include "lib/aurora/aurora_shm.h"
#include <sys/shm.h>
#include "ocudu/aurora/aurora.h"
#include <cstring>
#include <gtest/gtest.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;

TEST(aurora_e2e, publisher_and_thread_collector_fill_shared_memory)
{
  aurora::config cfg;
  cfg.enabled                  = true;
  cfg.type                     = aurora::ran_type::du;
  cfg.collection_interval      = std::chrono::milliseconds{50};
  cfg.shm_key                  = 0x7b000000U | (static_cast<uint32_t>(::getpid()) & 0xffffffU);
  cfg.thread_metrics_enabled   = true;
  cfg.thread_metrics_backend   = aurora::thread_backend::procfs;
  cfg.thread_metrics_whitelist = "aurora_reset,aurora_e2e_busy";
  cfg.anomaly_enabled          = false;
  ASSERT_EQ(aurora::start(cfg), 0);

  aurora_metrics* shm = aurora_get_metrics_by_key(static_cast<key_t>(cfg.shm_key));
  ASSERT_NE(shm, nullptr);
  EXPECT_EQ(shm->ran_type, RAN_DU);

  std::atomic<bool> run{true};
  std::thread       busy([&run]() {
    ::pthread_setname_np(::pthread_self(), "aurora_e2e_busy");
    volatile uint64_t x = 0;
    while (run.load()) {
      for (int i = 0; i != 100000; ++i) {
        x = x + i;
      }
      std::this_thread::yield();
    }
  });

  // Keep producing traffic for several intervals; at least one published interval must carry it.
  uint64_t seen_bytes = 0;
  uint64_t first_ts   = shm->timestamp_us;
  for (int i = 0; i != 20 && seen_bytes == 0; ++i) {
    aurora::record_flow(aurora::flow::f1u_rlc, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    seen_bytes = shm->f1u_rlc_size;
  }
  EXPECT_GT(seen_bytes, 0U);
  EXPECT_GT(shm->timestamp_us, first_ts);

  // Thread collector discovered the whitelisted threads.
  bool found_busy = false;
  for (int i = 0; i != 40 && !found_busy; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    for (unsigned t = 0; t != AURORA_MAX_MONITORED_THREADS; ++t) {
      const aurora_thread_metrics_t& tm = shm->thread_stats.threads[t];
      if (tm.valid && std::strcmp(tm.thread_name, "aurora_e2e_busy") == 0) {
        found_busy = true;
        EXPECT_EQ(tm.pid, ::getpid());
      }
    }
  }
  EXPECT_TRUE(found_busy);

  run = false;
  busy.join();
  ::shmdt(shm);
  aurora::stop();
}
