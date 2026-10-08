#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "anomaly_module.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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
  atomic_uint_fast32_t mac_delay_us;
  atomic_uint_fast32_t pdcp_delay_us;
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
    .running = ATOMIC_VAR_INIT(false),
    .listen_fd = -1,
    .listen_port = 0,
    .ran_type = 0,
    .role_name = "UNKNOWN",
    .bind_address = "127.0.0.1",
    .listener_thread = 0,
    .leak_thread = 0,
    .listener_thread_started = ATOMIC_VAR_INIT(false),
    .leak_thread_started = ATOMIC_VAR_INIT(false),
    .state = {
      .l1_until_ms = ATOMIC_VAR_INIT(0),
      .mac_until_ms = ATOMIC_VAR_INIT(0),
      .pdcp_until_ms = ATOMIC_VAR_INIT(0),
      .queue_until_ms = ATOMIC_VAR_INIT(0),
      .memory_until_ms = ATOMIC_VAR_INIT(0),
      .l1_delay_us = ATOMIC_VAR_INIT(0),
      .mac_delay_us = ATOMIC_VAR_INIT(0),
      .pdcp_delay_us = ATOMIC_VAR_INIT(0),
      .queue_jitter_us = ATOMIC_VAR_INIT(0),
      .leak_bytes_per_sec = ATOMIC_VAR_INIT(0),
    },
    .leak_lock = PTHREAD_MUTEX_INITIALIZER,
    .leak_head = NULL,
    .leak_total_bytes = 0,
};

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
    } else if (!strcasecmp(key, "token")) {
      snprintf(cmd->token, sizeof(cmd->token), "%s", val);
    } else {
      return -1;
    }
  }

  return 0;
}

static void clear_type(anomaly_type_t type)
{
  switch (type) {
    case ANOM_TYPE_L1_THREAD_CONTENTION:
      atomic_store(&g_runtime.state.l1_until_ms, 0);
      break;
    case ANOM_TYPE_MAC_CONTENTION:
      atomic_store(&g_runtime.state.mac_until_ms, 0);
      break;
    case ANOM_TYPE_PDCP_CONTENTION:
      atomic_store(&g_runtime.state.pdcp_until_ms, 0);
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
      atomic_store(&g_runtime.state.queue_until_ms, 0);
      atomic_store(&g_runtime.state.memory_until_ms, 0);
      anomaly_free_all_leaks();
      break;
    default:
      break;
  }
}

static void set_type(anomaly_type_t type, const parsed_cmd_t *cmd)
{
  uint64_t now = anomaly_now_ms();
  uint64_t until = now + (uint64_t)anomaly_clamp_u32(cmd->duration_ms, (uint32_t)MAX_ANOMALY_DURATION_MS);

  switch (type) {
    case ANOM_TYPE_L1_THREAD_CONTENTION:
      atomic_store(&g_runtime.state.l1_delay_us, anomaly_clamp_u32(cmd->delay_us, 500000));
      atomic_store(&g_runtime.state.l1_until_ms, until);
      break;
    case ANOM_TYPE_MAC_CONTENTION:
      atomic_store(&g_runtime.state.mac_delay_us, anomaly_clamp_u32(cmd->delay_us, 500000));
      atomic_store(&g_runtime.state.mac_until_ms, until);
      break;
    case ANOM_TYPE_PDCP_CONTENTION:
      atomic_store(&g_runtime.state.pdcp_delay_us, anomaly_clamp_u32(cmd->delay_us, 500000));
      atomic_store(&g_runtime.state.pdcp_until_ms, until);
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
  int mac = (atomic_load(&g_runtime.state.mac_until_ms) > now) ? 1 : 0;
  int pdcp = (atomic_load(&g_runtime.state.pdcp_until_ms) > now) ? 1 : 0;
  int queue = (atomic_load(&g_runtime.state.queue_until_ms) > now) ? 1 : 0;
  int mem = (atomic_load(&g_runtime.state.memory_until_ms) > now) ? 1 : 0;

  pthread_mutex_lock(&g_runtime.leak_lock);
  leak_total = g_runtime.leak_total_bytes;
  pthread_mutex_unlock(&g_runtime.leak_lock);

  snprintf(out,
           out_sz,
           "OK role=%s port=%u active={L1:%d,MAC:%d,PDCP:%d,QUEUE:%d,MEM:%d} leak_bytes=%zu\n",
           g_runtime.role_name,
           g_runtime.listen_port,
           l1,
           mac,
           pdcp,
           queue,
           mem,
           leak_total);
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
    snprintf(out, out_sz, "OK PONG role=%s port=%u\n", g_runtime.role_name, g_runtime.listen_port);
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

    set_type(type, &cmd);
    snprintf(out,
             out_sz,
             "OK applied=1 role=%s type=%s duration_ms=%u\n",
             g_runtime.role_name,
             cmd.type,
             cmd.duration_ms);
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

    FD_ZERO(&rfds);
    FD_SET(g_runtime.listen_fd, &rfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;

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

void anomaly_module_hook_mac(void)
{
  apply_delay_if_active(&g_runtime.state.l1_until_ms, &g_runtime.state.l1_delay_us);
  apply_delay_if_active(&g_runtime.state.mac_until_ms, &g_runtime.state.mac_delay_us);

  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}

void anomaly_module_hook_rlc(void)
{
  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}

void anomaly_module_hook_pdcp(void)
{
  apply_delay_if_active(&g_runtime.state.pdcp_until_ms, &g_runtime.state.pdcp_delay_us);

  if (atomic_load(&g_runtime.state.queue_until_ms) > anomaly_now_ms()) {
    uint32_t max_jitter = atomic_load(&g_runtime.state.queue_jitter_us);
    uint32_t jitter = (max_jitter == 0) ? 0 : (uint32_t)(anomaly_now_ms() % (uint64_t)(max_jitter + 1));
    anomaly_sleep_us(jitter);
  }
}
