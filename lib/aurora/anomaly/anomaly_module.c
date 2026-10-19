#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* cpu_set_t, sched_getaffinity() */
#endif

#include "anomaly_module.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
  ANOM_TYPE_UNKNOWN = 0,
  ANOM_TYPE_L1_THREAD_CONTENTION,
  ANOM_TYPE_MAC_CONTENTION,
  ANOM_TYPE_PDCP_CONTENTION,
  ANOM_TYPE_QUEUE_TUNING,
  ANOM_TYPE_MEMORY_LEAK,
  ANOM_TYPE_ALL
} anomaly_type_t;

typedef struct leak_block_s {
  void *ptr;
  size_t size;
  struct leak_block_s *next;
} leak_block_t;

typedef struct {
  atomic_uint_fast64_t l1_until_ms;
  atomic_uint_fast64_t mac_until_ms;
  atomic_uint_fast64_t pdcp_until_ms;
  atomic_uint_fast64_t queue_until_ms;
  atomic_uint_fast64_t memory_until_ms;

  atomic_uint_fast32_t l1_delay_us;
  atomic_uint_fast32_t queue_jitter_us;
  atomic_uint_fast32_t leak_bytes_per_sec;
} anomaly_state_t;

typedef struct {
  atomic_bool running;
  int listen_fd;
  uint32_t listen_port;
  int ran_type;
  char role_name[16];
  char bind_address[64];

  uint64_t started_us; /* wall clock at anomaly_module_start(), i.e. AURORA start-up */
  pthread_t listener_thread;
  pthread_t leak_thread;
  atomic_bool listener_thread_started;
  atomic_bool leak_thread_started;

  anomaly_state_t state;

  pthread_mutex_t leak_lock;
  leak_block_t *leak_head;
  size_t leak_total_bytes;
} anomaly_runtime_t;

static anomaly_runtime_t g_runtime = {
    .running = false,
    .listen_fd = -1,
    .listen_port = 0,
    .ran_type = 0,
    .role_name = "UNKNOWN",
    .bind_address = "127.0.0.1",
    .listener_thread = 0,
    .leak_thread = 0,
    .listener_thread_started = false,
    .leak_thread_started = false,
    .state = {
      .l1_until_ms = 0,
      .mac_until_ms = 0,
      .pdcp_until_ms = 0,
      .queue_until_ms = 0,
      .memory_until_ms = 0,
      .l1_delay_us = 0,
      .queue_jitter_us = 0,
      .leak_bytes_per_sec = 0,
    },
    .leak_lock = PTHREAD_MUTEX_INITIALIZER,
    .leak_head = NULL,
    .leak_total_bytes = 0,
};

/* OCUDU: the hooks run on the scheduler slot / RLC / PDCP data paths. This flag lets them return after a single
 * relaxed load while no anomaly is armed (no clock read). Set by SET, cleared by CLEAR type=ALL / stop. Both run on
 * the listener thread, so arming and disarming never race each other. */
static atomic_bool g_armed = false;

/* Label set by the sidecar ("LABEL value=..."). Seqlock: the listener thread is the only writer, the AURORA publisher
 * reads it once per collection interval and retries while a write is in progress, so neither ever blocks. */
static atomic_uint g_label_seq = 0;
static char g_label[ANOMALY_LABEL_LEN] = "normal";

static void label_store(const char *value)
{
  char buf[ANOMALY_LABEL_LEN];
  snprintf(buf, sizeof(buf), "%s", value);
  atomic_fetch_add_explicit(&g_label_seq, 1, memory_order_relaxed); /* odd: write in progress */
  atomic_thread_fence(memory_order_release);
  for (size_t i = 0; i < sizeof(g_label); i++)
    __atomic_store_n(&g_label[i], buf[i], __ATOMIC_RELAXED);
  atomic_fetch_add_explicit(&g_label_seq, 1, memory_order_release);
}

void anomaly_module_get_label(char *out, size_t out_sz)
{
  char buf[ANOMALY_LABEL_LEN];
  unsigned seq0;
  unsigned seq1;

  do {
    seq0 = atomic_load_explicit(&g_label_seq, memory_order_acquire);
    for (size_t i = 0; i < sizeof(buf); i++)
      buf[i] = __atomic_load_n(&g_label[i], __ATOMIC_RELAXED);
    atomic_thread_fence(memory_order_acquire);
    seq1 = atomic_load_explicit(&g_label_seq, memory_order_relaxed);
  } while ((seq0 & 1U) || seq0 != seq1);
  buf[sizeof(buf) - 1] = '\0';
  snprintf(out, out_sz, "%s", buf);
}

/* Lower-case letters, digits, '_', '+', '-': safe in CSV cells and InfluxDB string fields. */
static bool label_is_valid(const char *value)
{
  size_t n = strlen(value);
  return n > 0 && n < ANOMALY_LABEL_LEN && strspn(value, "abcdefghijklmnopqrstuvwxyz0123456789_+-") == n;
}

/* OCUDU: MAC_CONTENTION and PDCP_CONTENTION run stress-ng on the CPUs of the dedicated "mac_sched#<cell>" and
 * "pdcp" workers, with their scheduling policy and priority, so that the stressor competes with the worker for the
 * CPU (SCHED_FIFO at the same priority: each runs until it blocks). The load ramps linearly from load_min to
 * load_max over the anomaly duration, one step every step_ms; stress-ng cannot change its load while running, so
 * every step restarts it. Only the listener thread starts and stops the stressors (anomaly_module_stop() joins it
 * first), so this state needs no lock. */
typedef struct {
  pid_t pid; /* stress-ng process (group leader), 0 when idle */
  char cpus[128];
  cpu_set_t cpu_set;
  int policy;
  int prio;
  uint32_t slice_ms;
  char method[32]; /* stress-ng --cpu-method */
  uint32_t load_min;
  uint32_t load_max;
  uint32_t load; /* load of the running step */
  uint32_t nof_steps;
  uint32_t step;
  uint64_t step_ms;
  uint64_t start_ms;
} stressor_t;

static stressor_t g_mac_stressor;
static stressor_t g_pdcp_stressor;

static const uint64_t MAX_ANOMALY_DURATION_MS = 3600000ULL;
static const size_t MAX_LEAK_TOTAL_BYTES = (size_t)512 * 1024 * 1024;

static uint64_t anomaly_now_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

static void anomaly_sleep_us(uint32_t delay_us)
{
  if (delay_us == 0)
    return;

  struct timespec ts;
  ts.tv_sec = delay_us / 1000000U;
  ts.tv_nsec = (long)(delay_us % 1000000U) * 1000L;
  nanosleep(&ts, NULL);
}

const char *anomaly_module_role_name(int ran_type)
{
  switch (ran_type) {
    case 0: return "RAN_GNB";
    case 1: return "RAN_DU";
    case 2: return "RAN_CU";
    case 3: return "RAN_CU_CP";
    case 4: return "RAN_CU_UP";
    default: return "UNKNOWN";
  }
}

uint32_t anomaly_module_default_port_for_ran(int ran_type)
{
  if (ran_type < 0 || ran_type > 4)
    return 47010;
  return (uint32_t)(47010 + ran_type);
}

static void anomaly_free_all_leaks(void)
{
  pthread_mutex_lock(&g_runtime.leak_lock);
  leak_block_t *node = g_runtime.leak_head;
  while (node) {
    leak_block_t *next = node->next;
    free(node->ptr);
    free(node);
    node = next;
  }
  g_runtime.leak_head = NULL;
  g_runtime.leak_total_bytes = 0;
  pthread_mutex_unlock(&g_runtime.leak_lock);
}

static bool anomaly_target_matches(const char *target)
{
  if (!target || target[0] == '\0')
    return true;
  if (!strcasecmp(target, "ALL"))
    return true;
  return !strcasecmp(target, g_runtime.role_name);
}

static anomaly_type_t anomaly_type_from_string(const char *type)
{
  if (!type)
    return ANOM_TYPE_UNKNOWN;
  if (!strcasecmp(type, "L1_THREAD_CONTENTION"))
    return ANOM_TYPE_L1_THREAD_CONTENTION;
  if (!strcasecmp(type, "MAC_CONTENTION"))
    return ANOM_TYPE_MAC_CONTENTION;
  if (!strcasecmp(type, "PDCP_CONTENTION"))
    return ANOM_TYPE_PDCP_CONTENTION;
  if (!strcasecmp(type, "QUEUE_TUNING"))
    return ANOM_TYPE_QUEUE_TUNING;
  if (!strcasecmp(type, "MEMORY_LEAK"))
    return ANOM_TYPE_MEMORY_LEAK;
  if (!strcasecmp(type, "ALL"))
    return ANOM_TYPE_ALL;
  return ANOM_TYPE_UNKNOWN;
}

static uint32_t anomaly_clamp_u32(uint32_t value, uint32_t max)
{
  if (value > max)
    return max;
  return value;
}

typedef struct {
  char cmd[16];
  char target[32];
  char type[64];
  char token[128];
  uint32_t duration_ms;
  uint32_t delay_us;
  uint32_t jitter_us;
  uint32_t bytes_per_sec;
  uint32_t load;      /* stressor CPU load in percent, when no ramp is given */
  uint32_t load_min;  /* ramp start, 0 = load */
  uint32_t load_max;  /* ramp end, 0 = load_min */
  uint32_t step_ms;   /* ramp step */
  uint32_t slice_ms;  /* stressor busy slice */
  int32_t prio_offset; /* stressor priority relative to the worker */
  int32_t cell;       /* MAC: cell index, -1 for every cell */
  char method[32];    /* stressor stress-ng --cpu-method */
  char value[64];     /* LABEL value */
} parsed_cmd_t;

static int parse_u32(const char *s, uint32_t *out)
{
  char *end = NULL;
  unsigned long v;
  if (!s || !out)
    return -1;
  errno = 0;
  v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFUL)
    return -1;
  *out = (uint32_t)v;
  return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
  char *end = NULL;
  long v;
  if (!s || !out)
    return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < -2147483647L - 1 || v > 2147483647L)
    return -1;
  *out = (int32_t)v;
  return 0;
}

static int parse_command_line(const char *line, parsed_cmd_t *cmd)
{
  char buf[512];
  size_t line_len;
  char *saveptr = NULL;
  char *token;

  if (!line || !cmd)
    return -1;

  memset(cmd, 0, sizeof(*cmd));
  snprintf(cmd->target, sizeof(cmd->target), "ALL");
  cmd->duration_ms = 10000;
  cmd->delay_us = 200;
  cmd->jitter_us = 200;
  cmd->bytes_per_sec = 1048576;
  cmd->load = 50;
  cmd->load_min = 0;
  cmd->load_max = 0;
  cmd->step_ms = 1000;
  cmd->slice_ms = 1;
  cmd->prio_offset = 0;
  cmd->cell = -1;
  /* An empty loop: short operations keep the busy slices close to slice_ms (the default "all" includes methods
   * whose single operation takes several ms). */
  snprintf(cmd->method, sizeof(cmd->method), "loop");

  line_len = strnlen(line, sizeof(buf) - 1);
  memcpy(buf, line, line_len);
  buf[line_len] = '\0';
  token = strtok_r(buf, " \t\r\n", &saveptr);
  if (!token)
    return -1;

  snprintf(cmd->cmd, sizeof(cmd->cmd), "%s", token);

  for (;;) {
    char *eq;
    char *key;
    char *val;

    token = strtok_r(NULL, " \t\r\n", &saveptr);
    if (!token)
      break;

    eq = strchr(token, '=');
    if (!eq)
      return -1;
    *eq = '\0';
    key = token;
    val = eq + 1;

    if (!strcasecmp(key, "target")) {
      snprintf(cmd->target, sizeof(cmd->target), "%s", val);
    } else if (!strcasecmp(key, "type")) {
      snprintf(cmd->type, sizeof(cmd->type), "%s", val);
    } else if (!strcasecmp(key, "duration_ms")) {
      if (parse_u32(val, &cmd->duration_ms) != 0)
        return -1;
    } else if (!strcasecmp(key, "delay_us")) {
      if (parse_u32(val, &cmd->delay_us) != 0)
        return -1;
    } else if (!strcasecmp(key, "jitter_us")) {
      if (parse_u32(val, &cmd->jitter_us) != 0)
        return -1;
    } else if (!strcasecmp(key, "bytes_per_sec")) {
      if (parse_u32(val, &cmd->bytes_per_sec) != 0)
        return -1;
    } else if (!strcasecmp(key, "load")) {
      if (parse_u32(val, &cmd->load) != 0 || cmd->load == 0 || cmd->load > 100)
        return -1;
    } else if (!strcasecmp(key, "load_min")) {
      if (parse_u32(val, &cmd->load_min) != 0 || cmd->load_min == 0 || cmd->load_min > 100)
        return -1;
    } else if (!strcasecmp(key, "load_max")) {
      if (parse_u32(val, &cmd->load_max) != 0 || cmd->load_max == 0 || cmd->load_max > 100)
        return -1;
    } else if (!strcasecmp(key, "step_ms")) {
      if (parse_u32(val, &cmd->step_ms) != 0 || cmd->step_ms < 100)
        return -1;
    } else if (!strcasecmp(key, "slice_ms")) {
      if (parse_u32(val, &cmd->slice_ms) != 0 || cmd->slice_ms == 0 || cmd->slice_ms > 100)
        return -1;
    } else if (!strcasecmp(key, "prio_offset")) {
      if (parse_i32(val, &cmd->prio_offset) != 0 || cmd->prio_offset < -10 || cmd->prio_offset > 10)
        return -1;
    } else if (!strcasecmp(key, "cell")) {
      if (parse_i32(val, &cmd->cell) != 0 || cmd->cell < 0)
        return -1;
    } else if (!strcasecmp(key, "method")) {
      if (val[0] == '\0' || strlen(val) >= sizeof(cmd->method) ||
          strspn(val, "abcdefghijklmnopqrstuvwxyz0123456789") != strlen(val))
        return -1;
      snprintf(cmd->method, sizeof(cmd->method), "%s", val);
    } else if (!strcasecmp(key, "value")) {
      snprintf(cmd->value, sizeof(cmd->value), "%s", val);
    } else if (!strcasecmp(key, "token")) {
      snprintf(cmd->token, sizeof(cmd->token), "%s", val);
    } else {
      return -1;
    }
  }

  return 0;
}

/* Worker threads of this process that a MAC_CONTENTION / PDCP_CONTENTION stressor targets. */
typedef struct {
  cpu_set_t cpus;     /* union of the affinities of the matched threads */
  int nof_threads;    /* matched threads */
  int policy;         /* policy and priority of the highest-priority matched thread */
  int prio;
  int nof_shared_rt;  /* other real-time threads of this process allowed on those CPUs */
} worker_target_t;

static bool read_thread_comm(const char *tid, char *out, size_t out_sz)
{
  char path[288];
  ssize_t n;
  int fd;

  snprintf(path, sizeof(path), "/proc/self/task/%s/comm", tid);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  n = read(fd, out, out_sz - 1);
  close(fd);
  if (n <= 0)
    return false;
  out[n] = '\0';
  out[strcspn(out, "\n")] = '\0';
  return true;
}

static bool worker_name_matches(anomaly_type_t type, int32_t cell, const char *comm)
{
  if (type == ANOM_TYPE_PDCP_CONTENTION)
    return strcmp(comm, "pdcp") == 0;
  if (strncmp(comm, "mac_sched#", 10) != 0)
    return false;
  if (cell < 0)
    return true;
  char name[32];
  snprintf(name, sizeof(name), "mac_sched#%d", (int)cell);
  return strcmp(comm, name) == 0;
}

static bool policy_is_rt(int policy)
{
  return policy == SCHED_FIFO || policy == SCHED_RR;
}

/* Scans /proc/self/task for the worker threads (names set by the worker_manager). Returns the number of matches. */
static int find_worker_target(anomaly_type_t type, int32_t cell, worker_target_t *t)
{
  DIR *dir;
  struct dirent *de;

  memset(t, 0, sizeof(*t));
  CPU_ZERO(&t->cpus);
  t->policy = SCHED_OTHER;
  t->prio = -1;

  /* Pass 1: the worker threads. Pass 2: other real-time threads sharing their CPUs (e.g. the main thread pool when
   * the worker has no CPU mask of its own and falls back to the pool affinity). */
  for (int pass = 0; pass < 2; pass++) {
    dir = opendir("/proc/self/task");
    if (!dir)
      return 0;
    while ((de = readdir(dir)) != NULL) {
      char comm[32];
      struct sched_param sp;
      cpu_set_t set;
      pid_t tid;
      int policy;

      if (de->d_name[0] < '0' || de->d_name[0] > '9')
        continue;
      if (!read_thread_comm(de->d_name, comm, sizeof(comm)))
        continue;
      tid = (pid_t)atoi(de->d_name);
      bool match = worker_name_matches(type, cell, comm);
      if ((pass == 0) != match)
        continue;
      if (sched_getaffinity(tid, sizeof(set), &set) != 0)
        continue;
      policy = sched_getscheduler(tid);
      if (policy < 0 || sched_getparam(tid, &sp) != 0)
        continue;

      if (pass == 0) {
        CPU_OR(&t->cpus, &t->cpus, &set);
        t->nof_threads++;
        if (sp.sched_priority > t->prio) {
          t->prio = sp.sched_priority;
          t->policy = policy;
        }
      } else if (policy_is_rt(policy)) {
        cpu_set_t common;
        CPU_AND(&common, &set, &t->cpus);
        if (CPU_COUNT(&common) > 0)
          t->nof_shared_rt++;
      }
    }
    closedir(dir);
    if (t->nof_threads == 0)
      break;
  }
  return t->nof_threads;
}

static void format_cpu_list(const cpu_set_t *set, char *out, size_t out_sz)
{
  size_t pos = 0;
  int cpu = 0;

  out[0] = '\0';
  while (cpu < CPU_SETSIZE) {
    int last;
    int n;

    if (!CPU_ISSET(cpu, set)) {
      cpu++;
      continue;
    }
    last = cpu;
    while (last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, set))
      last++;
    if (last == cpu)
      n = snprintf(out + pos, out_sz - pos, "%s%d", pos ? "," : "", cpu);
    else
      n = snprintf(out + pos, out_sz - pos, "%s%d-%d", pos ? "," : "", cpu, last);
    if (n < 0 || (size_t)n >= out_sz - pos)
      break;
    pos += (size_t)n;
    cpu = last + 1;
  }
}

static const char *policy_name(int policy)
{
  switch (policy) {
    case SCHED_FIFO: return "SCHED_FIFO";
    case SCHED_RR: return "SCHED_RR";
    case SCHED_OTHER: return "SCHED_OTHER";
    default: return "OTHER";
  }
}

extern char **environ;

/* Stressors being torn down: SIGTERMed (stress-ng then stops and reaps its own workers, which would otherwise end up
 * as zombies under PID 1), SIGKILLed when still alive after DYING_KILL_MS. Reaped by the listener loop. */
#define MAX_DYING_STRESSORS 8
#define DYING_KILL_MS 3000
typedef struct {
  pid_t pid;
  uint64_t kill_at_ms;
} dying_t;
static dying_t g_dying[MAX_DYING_STRESSORS];

/* block: wait until every dying stressor is gone (module stop). */
static void reap_dying(bool block)
{
  for (;;) {
    bool left = false;
    uint64_t now = anomaly_now_ms();
    for (int i = 0; i < MAX_DYING_STRESSORS; i++) {
      int status;
      dying_t *d = &g_dying[i];
      if (d->pid <= 0)
        continue;
      pid_t r = waitpid(d->pid, &status, WNOHANG);
      if (r == d->pid || (r < 0 && errno == ECHILD)) {
        kill(-d->pid, SIGKILL); /* workers that outlived their parent */
        d->pid = 0;
        continue;
      }
      if (now >= d->kill_at_ms) {
        kill(-d->pid, SIGKILL);
        d->kill_at_ms = UINT64_MAX;
      }
      left = true;
    }
    if (!block || !left)
      return;
    anomaly_sleep_us(10000);
  }
}

/* Sets the policy of every thread of every process in process group @pgid (stress-ng and its workers). */
static void set_group_sched(pid_t pgid, int policy, int prio)
{
  struct sched_param sp = {.sched_priority = prio};
  DIR *proc = opendir("/proc");
  struct dirent *de;

  if (!proc)
    return;
  /* The group leader first: workers it forks from now on inherit its policy. */
  sched_setscheduler(pgid, policy, &sp);
  while ((de = readdir(proc)) != NULL) {
    char path[300];
    char buf[512];
    const char *p;
    ssize_t n;
    int fd;
    int pgrp = -1;

    if (de->d_name[0] < '1' || de->d_name[0] > '9')
      continue;
    snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
      continue;
    buf[n] = '\0';
    /* "pid (comm) state ppid pgrp ...": comm may contain spaces, so parse after the last ')'. */
    p = strrchr(buf, ')');
    if (!p || sscanf(p + 1, " %*c %*d %d", &pgrp) != 1 || pgrp != pgid)
      continue;

    snprintf(path, sizeof(path), "/proc/%s/task", de->d_name);
    DIR *tasks = opendir(path);
    struct dirent *te;
    if (!tasks)
      continue;
    while ((te = readdir(tasks)) != NULL) {
      if (te->d_name[0] >= '1' && te->d_name[0] <= '9')
        sched_setscheduler((pid_t)atoi(te->d_name), policy, &sp);
    }
    closedir(tasks);
  }
  closedir(proc);
}

/* Drops the stressor to SCHED_OTHER, so that its teardown never competes with the worker, then stops it. */
static void stressor_retire(pid_t pid)
{
  if (pid <= 0)
    return;
  set_group_sched(pid, SCHED_OTHER, 0);
  kill(-pid, SIGTERM);
  for (int i = 0; i < MAX_DYING_STRESSORS; i++) {
    if (g_dying[i].pid <= 0) {
      g_dying[i].pid = pid;
      g_dying[i].kill_at_ms = anomaly_now_ms() + DYING_KILL_MS;
      return;
    }
  }
  /* No free slot: wait for the oldest ones. */
  reap_dying(true);
  g_dying[0].pid = pid;
  g_dying[0].kill_at_ms = anomaly_now_ms() + DYING_KILL_MS;
}

/* Starts stress-ng with the current step load for the remaining anomaly time and raises it to the stressor policy
 * once it runs. It starts at SCHED_OTHER, so that its start-up (option parsing, forking the workers) does not
 * compete with the worker either; @prev, the stressor of the previous ramp step, keeps running meanwhile and is
 * retired just before the new one is raised (on failure too). Returns the new pid, or -1 with @err set. */
static pid_t stressor_spawn(const stressor_t *st, uint64_t remaining_ms, pid_t prev, char *err, size_t err_sz)
{
  const char *bin = getenv("AURORA_ANOMALY_STRESS_NG");
  char nof_cpus[16];
  char load[16];
  char slice[16];
  char timeout[24];
  struct sched_param sp = {.sched_priority = 0};
  posix_spawnattr_t attr;
  posix_spawn_file_actions_t files;
  sigset_t mask;
  cpu_set_t saved;
  bool affinity_saved;
  pid_t pid = 0;
  int status;
  int rc;

  if (!bin || bin[0] == '\0')
    bin = "stress-ng";

  snprintf(nof_cpus, sizeof(nof_cpus), "%d", CPU_COUNT(&st->cpu_set));
  snprintf(load, sizeof(load), "%u", st->load);
  snprintf(slice, sizeof(slice), "%u", st->slice_ms);
  /* Backstop only: the listener stops the stressor when the step or the anomaly ends, but if this process dies the
   * stressor must not keep competing with real-time threads for long. */
  snprintf(timeout, sizeof(timeout), "%llus", (unsigned long long)(remaining_ms / 1000U + 2U));

  char *argv[] = {(char *)bin,
                  "--cpu",
                  nof_cpus,
                  "--taskset",
                  (char *)st->cpus,
                  "--cpu-load",
                  load,
                  "--cpu-load-slice",
                  slice,
                  "--cpu-method",
                  (char *)st->method,
                  "--timeout",
                  timeout,
                  "--temp-path",
                  "/tmp",
                  "--quiet",
                  NULL};

  posix_spawnattr_init(&attr);
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  sigfillset(&mask);
  posix_spawnattr_setsigdefault(&attr, &mask);
  posix_spawnattr_setpgroup(&attr, 0);
  /* SCHED_OTHER: this thread may itself run with a real-time policy. */
  posix_spawnattr_setschedpolicy(&attr, SCHED_OTHER);
  posix_spawnattr_setschedparam(&attr, &sp);
  posix_spawnattr_setflags(&attr,
                           POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP |
                               POSIX_SPAWN_SETSCHEDULER);

  /* stress-ng output (e.g. its exit report) stays out of the RAN process console. */
  posix_spawn_file_actions_init(&files);
  posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  /* The child inherits the affinity of this thread. Narrow it to the target CPUs first, so that stress-ng never runs
   * on other CPUs (e.g. those of the L1 threads), even before applying --taskset. */
  affinity_saved = sched_getaffinity(0, sizeof(saved), &saved) == 0;
  sched_setaffinity(0, sizeof(st->cpu_set), &st->cpu_set);
  rc = posix_spawnp(&pid, bin, &files, &attr, argv, environ);
  if (affinity_saved)
    sched_setaffinity(0, sizeof(saved), &saved);
  posix_spawn_file_actions_destroy(&files);
  posix_spawnattr_destroy(&attr);

  if (rc != 0) {
    snprintf(err, err_sz, "spawn_%s_failed_%s", bin, strerror(rc));
    for (char *c = err; *c; c++)
      if (*c == ' ')
        *c = '_';
    stressor_retire(prev);
    return -1;
  }

  /* Start-up time, and catches a stress-ng that rejects its arguments (e.g. one too old for --cpu-load-slice). */
  anomaly_sleep_us(200000);
  if (waitpid(pid, &status, WNOHANG) == pid) {
    snprintf(err, err_sz, "stress-ng_exited_status_%d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    stressor_retire(prev);
    return -1;
  }
  stressor_retire(prev);
  set_group_sched(pid, st->policy, st->prio);
  if (sched_getscheduler(pid) != st->policy) {
    /* E.g. no CAP_SYS_NICE: at SCHED_OTHER the stressor would barely compete with a real-time worker. */
    snprintf(err, err_sz, "cannot_set_%s_on_stress-ng_%s", policy_name(st->policy), strerror(errno));
    for (char *c = err; *c; c++)
      if (*c == ' ')
        *c = '_';
    stressor_retire(pid);
    return -1;
  }
  return pid;
}

/* Reaps the stressor if it exited by itself (e.g. its --timeout backstop). */
static void stressor_reap(stressor_t *st)
{
  int status;
  if (st->pid > 0 && waitpid(st->pid, &status, WNOHANG) == st->pid)
    st->pid = 0;
}

static void stressor_stop(stressor_t *st)
{
  stressor_retire(st->pid);
  st->pid = 0;
}

static uint32_t stressor_step_load(const stressor_t *st, uint32_t step)
{
  if (st->nof_steps <= 1)
    return st->load_max;
  return st->load_min + (uint32_t)(((uint64_t)(st->load_max - st->load_min) * step) / (st->nof_steps - 1));
}

static stressor_t *stressor_for(anomaly_type_t type)
{
  return type == ANOM_TYPE_MAC_CONTENTION ? &g_mac_stressor : &g_pdcp_stressor;
}

static atomic_uint_fast64_t *stressor_until(anomaly_type_t type)
{
  return type == ANOM_TYPE_MAC_CONTENTION ? &g_runtime.state.mac_until_ms : &g_runtime.state.pdcp_until_ms;
}

/* SET type=MAC_CONTENTION|PDCP_CONTENTION. Replaces a stressor of the same type that is still running. */
static void start_contention(anomaly_type_t type, const parsed_cmd_t *cmd, char *out, size_t out_sz)
{
  stressor_t *st = stressor_for(type);
  worker_target_t t;
  char err[128];
  uint32_t duration = anomaly_clamp_u32(cmd->duration_ms, (uint32_t)MAX_ANOMALY_DURATION_MS);
  uint32_t load_min = cmd->load_min ? cmd->load_min : cmd->load;
  uint32_t load_max = cmd->load_max ? cmd->load_max : load_min;

  atomic_store(stressor_until(type), 0);
  stressor_stop(st);

  if (load_min > load_max) {
    snprintf(out, out_sz, "ERR code=BAD_ARG message=load_min_above_load_max\n");
    return;
  }
  if (duration == 0) {
    snprintf(out, out_sz, "ERR code=BAD_ARG message=duration_ms_is_0\n");
    return;
  }
  if (find_worker_target(type, cmd->cell, &t) == 0) {
    snprintf(out,
             out_sz,
             "ERR code=NO_WORKER message=no_%s_thread_enable_the_dedicated_worker\n",
             type == ANOM_TYPE_MAC_CONTENTION ? "mac_sched" : "pdcp");
    return;
  }

  memset(st, 0, sizeof(*st));
  st->cpu_set = t.cpus;
  format_cpu_list(&t.cpus, st->cpus, sizeof(st->cpus));
  st->policy = t.policy;
  st->prio = 0;
  if (policy_is_rt(t.policy)) {
    int prio = t.prio + cmd->prio_offset;
    int lo = sched_get_priority_min(t.policy);
    int hi = sched_get_priority_max(t.policy);
    st->prio = prio < lo ? lo : (prio > hi ? hi : prio);
  }
  st->slice_ms = cmd->slice_ms;
  snprintf(st->method, sizeof(st->method), "%s", cmd->method);
  st->load_min = load_min;
  st->load_max = load_max;
  st->step_ms = load_min == load_max ? duration : (cmd->step_ms < duration ? cmd->step_ms : duration);
  st->nof_steps = (uint32_t)((duration + st->step_ms - 1) / st->step_ms);
  st->step = 0;
  st->load = stressor_step_load(st, 0);

  st->pid = stressor_spawn(st, duration, 0, err, sizeof(err));
  if (st->pid <= 0) {
    st->pid = 0;
    snprintf(out, out_sz, "ERR code=STRESSOR_FAILED message=%s\n", err);
    return;
  }
  st->start_ms = anomaly_now_ms();
  atomic_store(stressor_until(type), st->start_ms + duration);

  snprintf(out,
           out_sz,
           "OK applied=1 role=%s type=%s duration_ms=%u threads=%d cpus=%s policy=%s worker_prio=%d stress_prio=%d "
           "load_min=%u load_max=%u step_ms=%llu steps=%u slice_ms=%u method=%s pid=%d shared_rt_threads=%d%s\n",
           g_runtime.role_name,
           cmd->type,
           duration,
           t.nof_threads,
           st->cpus,
           policy_name(t.policy),
           t.prio,
           st->prio,
           st->load_min,
           st->load_max,
           (unsigned long long)st->step_ms,
           st->nof_steps,
           st->slice_ms,
           st->method,
           (int)st->pid,
           t.nof_shared_rt,
           t.nof_shared_rt > 0 ? " warning=other_rt_threads_share_these_cpus" : "");
}

/* Called by the listener loop: advances the ramps, stops expired stressors and reaps those that exited. Returns the
 * time to the next step or expiry in ms (at most 1000). */
static uint64_t service_stressors(void)
{
  static const anomaly_type_t types[] = {ANOM_TYPE_MAC_CONTENTION, ANOM_TYPE_PDCP_CONTENTION};
  uint64_t next = 1000;

  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    stressor_t *st = stressor_for(types[i]);
    uint64_t until = atomic_load(stressor_until(types[i]));
    uint64_t now = anomaly_now_ms();

    reap_dying(false);
    stressor_reap(st);
    if (until == 0)
      continue;
    if (st->pid <= 0 || until <= now) {
      stressor_stop(st);
      atomic_store(stressor_until(types[i]), 0);
      continue;
    }

    uint64_t step_at = st->start_ms + (uint64_t)(st->step + 1) * st->step_ms;
    if (st->step + 1 < st->nof_steps && now >= step_at) {
      char err[128];
      st->step = (uint32_t)((now - st->start_ms) / st->step_ms);
      if (st->step >= st->nof_steps)
        st->step = st->nof_steps - 1;
      st->load = stressor_step_load(st, st->step);
      pid_t pid = stressor_spawn(st, until - now, st->pid, err, sizeof(err));
      if (pid <= 0) {
        st->pid = 0; /* retired by stressor_spawn() */
        fprintf(stderr, "ANOMALY: %s ramp step %u failed: %s\n", types[i] == ANOM_TYPE_MAC_CONTENTION ? "MAC" : "PDCP",
                st->step, err);
        atomic_store(stressor_until(types[i]), 0);
        continue;
      }
      st->pid = pid;
      now = anomaly_now_ms();
      step_at = st->start_ms + (uint64_t)(st->step + 1) * st->step_ms;
    }

    uint64_t due = (st->step + 1 < st->nof_steps && step_at < until) ? step_at : until;
    if (due <= now)
      next = 0;
    else if (due - now < next)
      next = due - now;
  }
  return next;
}

static void clear_type(anomaly_type_t type)
{
  switch (type) {
    case ANOM_TYPE_L1_THREAD_CONTENTION:
      atomic_store(&g_runtime.state.l1_until_ms, 0);
      break;
    case ANOM_TYPE_MAC_CONTENTION:
    case ANOM_TYPE_PDCP_CONTENTION:
      atomic_store(stressor_until(type), 0);
      stressor_stop(stressor_for(type));
      break;
    case ANOM_TYPE_QUEUE_TUNING:
      atomic_store(&g_runtime.state.queue_until_ms, 0);
      break;
    case ANOM_TYPE_MEMORY_LEAK:
      atomic_store(&g_runtime.state.memory_until_ms, 0);
      anomaly_free_all_leaks();
      break;
    case ANOM_TYPE_ALL:
      atomic_store(&g_runtime.state.l1_until_ms, 0);
      atomic_store(&g_runtime.state.mac_until_ms, 0);
      atomic_store(&g_runtime.state.pdcp_until_ms, 0);
      stressor_stop(&g_mac_stressor);
      stressor_stop(&g_pdcp_stressor);
      atomic_store(&g_runtime.state.queue_until_ms, 0);
      atomic_store(&g_runtime.state.memory_until_ms, 0);
      anomaly_free_all_leaks();
      atomic_store(&g_armed, false);
      break;
    default:
      break;
  }
}

static void set_type(anomaly_type_t type, const parsed_cmd_t *cmd)
{
  uint64_t now = anomaly_now_ms();
  uint64_t until = now + (uint64_t)anomaly_clamp_u32(cmd->duration_ms, (uint32_t)MAX_ANOMALY_DURATION_MS);
  atomic_store(&g_armed, true);

  switch (type) {
    case ANOM_TYPE_L1_THREAD_CONTENTION:
      atomic_store(&g_runtime.state.l1_delay_us, anomaly_clamp_u32(cmd->delay_us, 500000));
      atomic_store(&g_runtime.state.l1_until_ms, until);
      break;
    case ANOM_TYPE_QUEUE_TUNING:
      atomic_store(&g_runtime.state.queue_jitter_us, anomaly_clamp_u32(cmd->jitter_us, 500000));
      atomic_store(&g_runtime.state.queue_until_ms, until);
      break;
    case ANOM_TYPE_MEMORY_LEAK:
      atomic_store(&g_runtime.state.leak_bytes_per_sec, anomaly_clamp_u32(cmd->bytes_per_sec, 32U * 1024U * 1024U));
      atomic_store(&g_runtime.state.memory_until_ms, until);
      break;
    default:
      break;
  }
}

static int send_response(int fd, const char *response)
{
  size_t len;
  ssize_t sent;

  if (!response)
    return -1;
  len = strlen(response);
  sent = send(fd, response, len, 0);
  return (sent == (ssize_t)len) ? 0 : -1;
}

static bool command_is_authorized(const parsed_cmd_t *cmd)
{
  const char *required = getenv("AURORA_ANOMALY_TOKEN");
  if (!required || required[0] == '\0')
    return true;
  if (!cmd || cmd->token[0] == '\0')
    return false;
  return strcmp(cmd->token, required) == 0;
}

static void format_status(char *out, size_t out_sz)
{
  uint64_t now = anomaly_now_ms();
  size_t leak_total = 0;
  int l1 = (atomic_load(&g_runtime.state.l1_until_ms) > now) ? 1 : 0;
  int mac = (g_mac_stressor.pid > 0 && atomic_load(&g_runtime.state.mac_until_ms) > now) ? 1 : 0;
  int pdcp = (g_pdcp_stressor.pid > 0 && atomic_load(&g_runtime.state.pdcp_until_ms) > now) ? 1 : 0;
  int queue = (atomic_load(&g_runtime.state.queue_until_ms) > now) ? 1 : 0;
  int mem = (atomic_load(&g_runtime.state.memory_until_ms) > now) ? 1 : 0;
  char label[ANOMALY_LABEL_LEN];

  anomaly_module_get_label(label, sizeof(label));

  pthread_mutex_lock(&g_runtime.leak_lock);
  leak_total = g_runtime.leak_total_bytes;
  pthread_mutex_unlock(&g_runtime.leak_lock);

  snprintf(out,
           out_sz,
           "OK role=%s port=%u started_us=%llu label=%s active={L1:%d,MAC:%d,PDCP:%d,QUEUE:%d,MEM:%d} leak_bytes=%zu "
           "mac_stress_cpus=%s mac_stress_load=%u pdcp_stress_cpus=%s pdcp_stress_load=%u\n",
           g_runtime.role_name,
           g_runtime.listen_port,
           (unsigned long long)g_runtime.started_us,
           label,
           l1,
           mac,
           pdcp,
           queue,
           mem,
           leak_total,
           mac ? g_mac_stressor.cpus : "-",
           mac ? g_mac_stressor.load : 0,
           pdcp ? g_pdcp_stressor.cpus : "-",
           pdcp ? g_pdcp_stressor.load : 0);
}

static void process_one_command(const char *line, char *out, size_t out_sz)
{
  parsed_cmd_t cmd;
  anomaly_type_t type;

  if (parse_command_line(line, &cmd) != 0) {
    snprintf(out, out_sz, "ERR code=BAD_CMD message=Could_not_parse_command\n");
    return;
  }

  if (!strcasecmp(cmd.cmd, "PING")) {
    snprintf(out,
             out_sz,
             "OK PONG role=%s port=%u started_us=%llu\n",
             g_runtime.role_name,
             g_runtime.listen_port,
             (unsigned long long)g_runtime.started_us);
    return;
  }

  if (!strcasecmp(cmd.cmd, "STATUS")) {
    format_status(out, out_sz);
    return;
  }

  if (!strcasecmp(cmd.cmd, "SET")) {
    if (!command_is_authorized(&cmd)) {
      snprintf(out, out_sz, "ERR code=UNAUTHORIZED message=invalid_or_missing_token\n");
      return;
    }

    if (!anomaly_target_matches(cmd.target)) {
      snprintf(out,
               out_sz,
               "ERR code=ROLE_MISMATCH message=target_%s_not_local_%s\n",
               cmd.target,
               g_runtime.role_name);
      return;
    }

    type = anomaly_type_from_string(cmd.type);
    if (type == ANOM_TYPE_UNKNOWN || type == ANOM_TYPE_ALL) {
      snprintf(out, out_sz, "ERR code=BAD_ARG message=invalid_type\n");
      return;
    }

    if (type == ANOM_TYPE_MAC_CONTENTION || type == ANOM_TYPE_PDCP_CONTENTION) {
      start_contention(type, &cmd, out, out_sz);
      return;
    }

    set_type(type, &cmd);
    snprintf(out,
             out_sz,
             "OK applied=1 role=%s type=%s duration_ms=%u\n",
             g_runtime.role_name,
             cmd.type,
             cmd.duration_ms);
    return;
  }

  if (!strcasecmp(cmd.cmd, "LABEL")) {
    if (!command_is_authorized(&cmd)) {
      snprintf(out, out_sz, "ERR code=UNAUTHORIZED message=invalid_or_missing_token\n");
      return;
    }
    if (!label_is_valid(cmd.value)) {
      snprintf(out, out_sz, "ERR code=BAD_ARG message=label_must_be_1_to_31_chars_of_a-z0-9_+-\n");
      return;
    }
    char previous[ANOMALY_LABEL_LEN];
    anomaly_module_get_label(previous, sizeof(previous));
    label_store(cmd.value);
    snprintf(out,
             out_sz,
             "OK label=%s changed=%d role=%s\n",
             cmd.value,
             strcmp(previous, cmd.value) != 0 ? 1 : 0,
             g_runtime.role_name);
    return;
  }

  if (!strcasecmp(cmd.cmd, "CLEAR")) {
    if (!command_is_authorized(&cmd)) {
      snprintf(out, out_sz, "ERR code=UNAUTHORIZED message=invalid_or_missing_token\n");
      return;
    }

    if (!anomaly_target_matches(cmd.target)) {
      snprintf(out,
               out_sz,
               "ERR code=ROLE_MISMATCH message=target_%s_not_local_%s\n",
               cmd.target,
               g_runtime.role_name);
      return;
    }

    type = anomaly_type_from_string(cmd.type[0] ? cmd.type : "ALL");
    if (type == ANOM_TYPE_UNKNOWN) {
      snprintf(out, out_sz, "ERR code=BAD_ARG message=invalid_type\n");
      return;
    }

    clear_type(type);
    snprintf(out,
             out_sz,
             "OK cleared=1 role=%s type=%s\n",
             g_runtime.role_name,
             (cmd.type[0] ? cmd.type : "ALL"));
    return;
  }

  snprintf(out, out_sz, "ERR code=BAD_CMD message=unsupported_command\n");
}

static void *memory_leak_worker(void *arg)
{
  (void)arg;

  while (atomic_load(&g_runtime.running)) {
    uint64_t now = anomaly_now_ms();
    uint64_t until = atomic_load(&g_runtime.state.memory_until_ms);

    if (until > now) {
      uint32_t leak_bps = atomic_load(&g_runtime.state.leak_bytes_per_sec);
      if (leak_bps > 0) {
        pthread_mutex_lock(&g_runtime.leak_lock);
        if (g_runtime.leak_total_bytes + leak_bps <= MAX_LEAK_TOTAL_BYTES) {
          void *buf = malloc(leak_bps);
          if (buf) {
            leak_block_t *node = (leak_block_t *)malloc(sizeof(*node));
            if (node) {
              memset(buf, 0xAB, leak_bps);
              node->ptr = buf;
              node->size = leak_bps;
              node->next = g_runtime.leak_head;
              g_runtime.leak_head = node;
              g_runtime.leak_total_bytes += leak_bps;
            } else {
              free(buf);
            }
          }
        }
        pthread_mutex_unlock(&g_runtime.leak_lock);
      }
    }

    sleep(1);
  }

  return NULL;
}

static void handle_client(int client_fd)
{
  char input[1024];
  char line[1024];
  size_t line_pos = 0;
  ssize_t n;
  struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};

  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  while ((n = recv(client_fd, input, sizeof(input), 0)) > 0) {
    size_t i;
    for (i = 0; i < (size_t)n; i++) {
      char c = input[i];
      if (c == '\n' || c == '\r') {
        if (line_pos > 0) {
          char response[512];
          line[line_pos] = '\0';
          process_one_command(line, response, sizeof(response));
          (void)send_response(client_fd, response);
          close(client_fd);
          return;
        }
      } else if (line_pos + 1 < sizeof(line)) {
        line[line_pos++] = c;
      }
    }
  }

  close(client_fd);
}

static int create_listener_socket(void)
{
  int fd;
  int opt = 1;
  struct sockaddr_in addr;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
    close(fd);
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)g_runtime.listen_port);
  if (inet_pton(AF_INET, g_runtime.bind_address, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void *listener_worker(void *arg)
{
  (void)arg;

  while (atomic_load(&g_runtime.running)) {
    fd_set rfds;
    struct timeval tv;
    int ret;

    uint64_t next_ms = service_stressors();

    FD_ZERO(&rfds);
    FD_SET(g_runtime.listen_fd, &rfds);
    tv.tv_sec = (time_t)(next_ms / 1000U);
    tv.tv_usec = (suseconds_t)((next_ms % 1000U) * 1000U);

    ret = select(g_runtime.listen_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ret == 0)
      continue;

    if (FD_ISSET(g_runtime.listen_fd, &rfds)) {
      int client_fd = accept(g_runtime.listen_fd, NULL, NULL);
      if (client_fd >= 0)
        handle_client(client_fd);
    }
  }

  if (g_runtime.listen_fd >= 0) {
    close(g_runtime.listen_fd);
    g_runtime.listen_fd = -1;
  }

  return NULL;
}

static uint32_t env_u32_or_default(const char *env_name, uint32_t def)
{
  const char *v = getenv(env_name);
  uint32_t out;
  if (!v)
    return def;
  if (parse_u32(v, &out) != 0)
    return def;
  if (out == 0 || out > 65535)
    return def;
  return out;
}

int anomaly_module_start(const anomaly_module_config_t *cfg)
{
  uint32_t port;
  const char *bind;

  if (!cfg)
    return -1;

  if (atomic_load(&g_runtime.running))
    return 0;

  g_runtime.ran_type = cfg->ran_type;
  snprintf(g_runtime.role_name, sizeof(g_runtime.role_name), "%s", anomaly_module_role_name(cfg->ran_type));

  port = cfg->listen_port ? cfg->listen_port : anomaly_module_default_port_for_ran(cfg->ran_type);
  port = env_u32_or_default("AURORA_ANOMALY_PORT", port);
  if (port == 0 || port > 65535)
    return -1;
  g_runtime.listen_port = port;
  /* Started together with the AURORA publisher, so this is when its samples begin: anomaly-ctl times its injections
   * from it (aurora_reader's warm-up counts from the first sample). */
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    g_runtime.started_us = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
  }

  bind = getenv("AURORA_ANOMALY_BIND");
  if (!bind)
    bind = cfg->bind_address ? cfg->bind_address : "127.0.0.1";
  snprintf(g_runtime.bind_address, sizeof(g_runtime.bind_address), "%s", bind);

  g_runtime.listen_fd = create_listener_socket();
  if (g_runtime.listen_fd < 0)
    return -1;

  atomic_store(&g_runtime.running, true);

  if (pthread_create(&g_runtime.listener_thread, NULL, listener_worker, NULL) != 0) {
    atomic_store(&g_runtime.running, false);
    close(g_runtime.listen_fd);
    g_runtime.listen_fd = -1;
    return -1;
  }
  atomic_store(&g_runtime.listener_thread_started, true);

  printf("ANOMALY: listener ready on %s:%u for role=%s\n",
         g_runtime.bind_address,
         g_runtime.listen_port,
         g_runtime.role_name);

  if (!atomic_load(&g_runtime.leak_thread_started)) {
    if (pthread_create(&g_runtime.leak_thread, NULL, memory_leak_worker, NULL) == 0) {
      atomic_store(&g_runtime.leak_thread_started, true);
    }
  }

  return 0;
}

void anomaly_module_stop(void)
{
  atomic_store(&g_runtime.running, false);

  if (g_runtime.listen_fd >= 0)
    shutdown(g_runtime.listen_fd, SHUT_RDWR);

  if (atomic_exchange(&g_runtime.listener_thread_started, false))
    pthread_join(g_runtime.listener_thread, NULL);

  if (atomic_exchange(&g_runtime.leak_thread_started, false)) {
    pthread_join(g_runtime.leak_thread, NULL);
  }

  if (g_runtime.listen_fd >= 0) {
    close(g_runtime.listen_fd);
    g_runtime.listen_fd = -1;
  }

  clear_type(ANOM_TYPE_ALL);
  reap_dying(true);
}

static void apply_delay_if_active(atomic_uint_fast64_t *until_ms, atomic_uint_fast32_t *delay_us)
{
  uint64_t now = anomaly_now_ms();
  uint64_t until = atomic_load(until_ms);
  if (until > now) {
    uint32_t delay = atomic_load(delay_us);
    anomaly_sleep_us(delay);
  }
}

/* Returns false when no anomaly was set since the last CLEAR type=ALL. */
static bool anomaly_hooks_armed(void)
{
  return atomic_load_explicit(&g_armed, memory_order_relaxed);
}

void anomaly_module_hook_mac(void)
{
  if (!anomaly_hooks_armed())
    return;
  apply_delay_if_active(&g_runtime.state.l1_until_ms, &g_runtime.state.l1_delay_us);

  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}

void anomaly_module_hook_rlc(void)
{
  if (!anomaly_hooks_armed())
    return;
  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}

void anomaly_module_hook_pdcp(void)
{
  if (!anomaly_hooks_armed())
    return;

  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}
