/*
 * AURORA shared helpers used by both the in-process core and the standalone aurora_reader
 * (port of the corresponding functions in OAI openair2/AURORA/aurora.c).
 */

#include "aurora_shm.h"
#include <sys/shm.h>
#include <time.h>

static int aurora_get_hist_bin(uint32_t val)
{
  if (val < 64)
    return 0;
  int bin = 0;
  for (uint32_t v = val >> 6; v != 0; v >>= 1)
    bin++;
  return bin < AURORA_HIST_BINS ? bin : AURORA_HIST_BINS - 1;
}

static void aurora_hist_add(aurora_hist_t *hist, int bin, uint32_t val)
{
  hist->bins[bin]++;
  hist->count++;
  hist->sum += val;
  if (hist->count == 1 || val < hist->min_val)
    hist->min_val = val;
  if (val > hist->max_val)
    hist->max_val = val;
}

void aurora_hist_record(aurora_hist_t *hist, uint32_t val)
{
  aurora_hist_add(hist, aurora_get_hist_bin(val), val);
}

void aurora_hist_record_shift(aurora_hist_t *hist, uint32_t val, int shift)
{
  uint32_t bin = val >> shift;
  aurora_hist_add(hist, bin >= AURORA_HIST_BINS ? AURORA_HIST_BINS - 1 : (int)bin, val);
}

uint64_t aurora_get_current_timestamp_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

struct aurora_metrics *aurora_get_metrics_by_key(key_t shm_key)
{
  int id = shmget(shm_key, sizeof(struct aurora_metrics), 0);
  if (id < 0) {
    /* Attach to an existing segment regardless of its size. */
    id = shmget(shm_key, 0, 0);
    if (id < 0)
      return NULL;
  }
  /* Consumers only read: attach read-only (the producer creates the segment with mode 0644). */
  void *p = shmat(id, NULL, SHM_RDONLY);
  return p == (void *)-1 ? NULL : (struct aurora_metrics *)p;
}
