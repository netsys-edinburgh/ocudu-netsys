// Unit tests of the per-interval merge of the cumulative eBPF thread statistics into shared memory.

#include "aurora_thread_ebpf_merge.h"
#include <gtest/gtest.h>

namespace {

aurora_bpf_thread_stats make_totals(uint64_t events, uint64_t ctxsw, uint64_t interval_max, uint64_t epoch)
{
  aurora_bpf_thread_stats s{};
  s.hist_runtime[3]          = events;
  s.total_events             = events;
  s.voluntary_ctxsw          = ctxsw;
  s.involuntary_ctxsw        = ctxsw;
  s.irq_count                = ctxsw;
  s.cpu_migrations           = ctxsw;
  s.max_runtime_ns           = interval_max;
  s.interval_max_runtime_ns  = interval_max;
  s.interval_epoch           = epoch;
  s.last_cpu                 = 5;
  return s;
}

} // namespace

TEST(aurora_ebpf_merge, first_read_adds_the_totals)
{
  aurora_thread_metrics_t m{};
  aurora_bpf_thread_stats prev{};
  const auto              now = make_totals(10, 4, 900, 1);

  aurora_ebpf_merge_interval(&m, &prev, &now, 1);

  EXPECT_EQ(m.total_events, 10);
  EXPECT_EQ(m.hist_runtime.bin[3], 10);
  EXPECT_EQ(m.voluntary_ctxsw, 4);
  EXPECT_EQ(m.involuntary_ctxsw, 4);
  EXPECT_EQ(m.irq_count, 4);
  EXPECT_EQ(m.cpu_migrations, 4);
  EXPECT_EQ(m.max_runtime_ns, 900);
  EXPECT_EQ(m.cpu_id, 5);
  EXPECT_EQ(prev.total_events, 10);
}

TEST(aurora_ebpf_merge, next_reads_add_only_the_deltas)
{
  aurora_thread_metrics_t m{};
  aurora_bpf_thread_stats prev{};
  const auto              first = make_totals(10, 4, 900, 1);
  aurora_ebpf_merge_interval(&m, &prev, &first, 1);

  // The publisher resets the shared-memory slot between intervals.
  aurora_thread_metrics_t m2{};
  const auto              second = make_totals(25, 7, 300, 2);
  aurora_ebpf_merge_interval(&m2, &prev, &second, 2);

  EXPECT_EQ(m2.total_events, 15);
  EXPECT_EQ(m2.hist_runtime.bin[3], 15);
  EXPECT_EQ(m2.voluntary_ctxsw, 3);
  EXPECT_EQ(m2.cpu_migrations, 3);
  // The per-interval maximum, not the cumulative one.
  EXPECT_EQ(m2.max_runtime_ns, 300);
}

TEST(aurora_ebpf_merge, maximum_of_another_interval_is_ignored)
{
  aurora_thread_metrics_t m{};
  aurora_bpf_thread_stats prev{};
  const auto              first = make_totals(10, 4, 900, 1);
  aurora_ebpf_merge_interval(&m, &prev, &first, 1);

  // The thread did not run during interval 2: its entry still carries the maximum of interval 1.
  aurora_thread_metrics_t m2{};
  aurora_ebpf_merge_interval(&m2, &prev, &first, 2);

  EXPECT_EQ(m2.total_events, 0);
  EXPECT_EQ(m2.max_runtime_ns, 0);
}

TEST(aurora_ebpf_merge, recreated_entry_restarts_the_deltas)
{
  aurora_thread_metrics_t m{};
  aurora_bpf_thread_stats prev = make_totals(100, 50, 900, 1);
  const auto              now  = make_totals(8, 2, 400, 3);

  aurora_ebpf_merge_interval(&m, &prev, &now, 3);

  EXPECT_EQ(m.total_events, 8);
  EXPECT_EQ(m.voluntary_ctxsw, 2);
  EXPECT_EQ(m.max_runtime_ns, 400);
}

TEST(aurora_ebpf_merge, unset_last_cpu_keeps_the_known_cpu)
{
  aurora_thread_metrics_t m{};
  m.cpu_id = 2;
  aurora_bpf_thread_stats prev{};
  auto                    now = make_totals(1, 0, 10, 1);
  now.last_cpu                = AURORA_BPF_CPU_UNSET;

  aurora_ebpf_merge_interval(&m, &prev, &now, 1);

  EXPECT_EQ(m.cpu_id, 2);
}
