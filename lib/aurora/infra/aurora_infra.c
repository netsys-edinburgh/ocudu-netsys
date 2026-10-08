/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*
 * aurora_infra.c — Main infrastructure metrics collector.
 *
 * Creates a separate SHM segment (Option B) for container + host + GPU
 * metrics.  Runs a collector thread that polls procfs/sysfs/cgroup at
 * per-category configurable intervals.
 *
 * Used by:
 *   - aurora_reader (sidecar) to init collection + export CSV
 *   - RAN process never touches this — fully decoupled via SHM key
 */

#include "aurora_infra.h"
#ifdef AURORA_INFRA_EBPF_ENABLED
#include "aurora_infra_ebpf.h"
#endif

#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>

/* ---------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------- */
static aurora_infra_metrics_t *g_infra_shm  = NULL;
static int                     g_infra_shmid = -1;
static aurora_infra_config_t   g_cfg;
static pthread_t               g_collector_tid;
static volatile int            g_running = 0;
static int                     g_ebpf_active = 0;

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Resolve per-category interval: if override is 0, use global default. */
static uint32_t resolve_interval(uint32_t category_ms, uint32_t default_ms)
{
    return (category_ms > 0) ? category_ms : default_ms;
}

/* Check if enough time elapsed since last poll for a category. */
static int should_poll(uint64_t last_us, uint32_t interval_ms, uint64_t now)
{
    if (last_us == 0) return 1;  /* First poll */
    uint64_t elapsed_ms = (now - last_us) / 1000;
    return elapsed_ms >= interval_ms;
}

/* ---------------------------------------------------------------
 * SHM management
 * --------------------------------------------------------------- */
static aurora_infra_metrics_t *create_infra_shm(key_t key)
{
    int shmid = shmget(key, sizeof(aurora_infra_metrics_t), IPC_CREAT | 0666);
    if (shmid < 0) {
        fprintf(stderr, "AURORA_INFRA: shmget(0x%x) failed: %s\n",
                key, strerror(errno));
        return NULL;
    }
    void *ptr = shmat(shmid, NULL, 0);
    if (ptr == (void *)-1) {
        fprintf(stderr, "AURORA_INFRA: shmat failed: %s\n", strerror(errno));
        return NULL;
    }
    g_infra_shmid = shmid;
    aurora_infra_metrics_t *m = (aurora_infra_metrics_t *)ptr;
    memset(m, 0, sizeof(*m));
    m->version = AURORA_INFRA_VERSION;
    return m;
}

/* ---------------------------------------------------------------
 * Collector thread
 *
 * Polls each metric category at its own interval.  The smallest
 * interval drives the sleep granularity (we wake every tick_ms
 * and check what is due).
 * --------------------------------------------------------------- */

/* Compute the GCD-based tick interval so we don't oversleep. */
static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = b; b = a % b; a = t; }
    return a;
}

static uint32_t compute_tick_ms(void)
{
    uint32_t def = g_cfg.infra_interval_ms;
    uint32_t intervals[] = {
        resolve_interval(g_cfg.intervals.cpu_interval_ms,     def),
        resolve_interval(g_cfg.intervals.memory_interval_ms,  def),
        resolve_interval(g_cfg.intervals.network_interval_ms, def),
        resolve_interval(g_cfg.intervals.disk_interval_ms,    def),
        resolve_interval(g_cfg.intervals.host_interval_ms,    def),
        resolve_interval(g_cfg.intervals.gpu_interval_ms,     def),
    };
    uint32_t g = intervals[0];
    for (int i = 1; i < 6; i++)
        g = gcd(g, intervals[i]);
    /* Clamp to at least 50ms to avoid busy-spinning */
    if (g < 50) g = 50;
    return g;
}

static void collect_container_cpu(uint64_t now)
{
    uint32_t iv = resolve_interval(g_cfg.intervals.cpu_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_cpu_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_container_cpu(&g_cfg, &g_infra_shm->container.cpu);
#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_ebpf_active)
        aurora_infra_ebpf_collect_container_cpu(&g_infra_shm->container.cpu);
#endif
    g_infra_shm->last_cpu_poll_us = now;
}

static void collect_container_mem(uint64_t now)
{
    uint32_t iv = resolve_interval(g_cfg.intervals.memory_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_mem_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_container_mem(&g_cfg, &g_infra_shm->container.mem);
#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_ebpf_active)
        aurora_infra_ebpf_collect_container_mem(&g_infra_shm->container.mem);
#endif
    g_infra_shm->last_mem_poll_us = now;
}

static void collect_container_net(uint64_t now)
{
    uint32_t iv = resolve_interval(g_cfg.intervals.network_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_net_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_container_net(&g_cfg, &g_infra_shm->container.net);
#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_ebpf_active)
        aurora_infra_ebpf_collect_container_net(&g_infra_shm->container.net);
#endif
    g_infra_shm->last_net_poll_us = now;
}

static void collect_container_disk(uint64_t now)
{
    uint32_t iv = resolve_interval(g_cfg.intervals.disk_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_disk_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_container_io(&g_cfg, &g_infra_shm->container.io);
#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_ebpf_active)
        aurora_infra_ebpf_collect_container_io(&g_infra_shm->container.io);
#endif
    g_infra_shm->last_disk_poll_us = now;
}

static void collect_container_proc(uint64_t now)
{
    /* Collect proc/FD at same interval as CPU (lightweight). OCUDU fix: uses its own poll timestamp -- OAI gated on
     * last_cpu_poll_us, which collect_container_cpu() had just refreshed, so these metrics were never collected. */
    static uint64_t last_proc_poll_us = 0;
    uint32_t iv = resolve_interval(g_cfg.intervals.cpu_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(last_proc_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_container_proc(&g_cfg, &g_infra_shm->container.proc);
    last_proc_poll_us = now;
}

static void collect_host_metrics(uint64_t now)
{
    uint32_t iv = resolve_interval(g_cfg.intervals.host_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_host_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_host_thermal(&g_cfg, &g_infra_shm->host.thermal);
    aurora_infra_procfs_collect_host_cpu(&g_cfg, &g_infra_shm->host.cpu);
    aurora_infra_procfs_collect_host_mem(&g_cfg, &g_infra_shm->host.mem);
    aurora_infra_procfs_collect_host_disk(&g_cfg, &g_infra_shm->host.disk);
    aurora_infra_procfs_collect_host_net(&g_cfg, &g_infra_shm->host.net);
    aurora_infra_procfs_collect_host_kernel(&g_cfg, &g_infra_shm->host.kernel);

#ifdef AURORA_INFRA_EBPF_ENABLED
    /* carrier_changes: eBPF supersedes the procfs/sysfs value per NIC when
     * active, same merge policy as the container-scoped counters above —
     * not summed, since the two count from different start points (sysfs:
     * since boot; eBPF: since this program attached). */
    if (g_ebpf_active) {
        for (uint32_t i = 0; i < g_infra_shm->host.net.num_nics; i++) {
            aurora_host_nic_t *nic = &g_infra_shm->host.net.nics[i];
            uint64_t ebpf_count;
            if (aurora_infra_ebpf_collect_host_nic_carrier(nic->name, &ebpf_count) == 0)
                nic->carrier_changes = ebpf_count;
        }
    }
#endif

    g_infra_shm->host.timestamp_us = now;
    g_infra_shm->last_host_poll_us = now;
}

static void collect_gpu_metrics(uint64_t now)
{
    if (!g_cfg.gpu_enabled) return;

    uint32_t iv = resolve_interval(g_cfg.intervals.gpu_interval_ms,
                                   g_cfg.infra_interval_ms);
    if (!should_poll(g_infra_shm->last_gpu_poll_us, iv, now))
        return;

    aurora_infra_procfs_collect_gpu(&g_cfg, &g_infra_shm->gpu);
    g_infra_shm->gpu.timestamp_us = now;
    g_infra_shm->last_gpu_poll_us = now;
}

static void *infra_collector_thread(void *arg)
{
    (void)arg;
    uint32_t tick_ms = compute_tick_ms();

    printf("AURORA_INFRA: Collector thread started (tick=%u ms, backend=%s)\n",
           tick_ms, g_ebpf_active ? "ebpf+procfs" : "procfs");
    printf("AURORA_INFRA: Intervals — cpu:%u mem:%u net:%u disk:%u host:%u gpu:%u ms\n",
           resolve_interval(g_cfg.intervals.cpu_interval_ms,     g_cfg.infra_interval_ms),
           resolve_interval(g_cfg.intervals.memory_interval_ms,  g_cfg.infra_interval_ms),
           resolve_interval(g_cfg.intervals.network_interval_ms, g_cfg.infra_interval_ms),
           resolve_interval(g_cfg.intervals.disk_interval_ms,    g_cfg.infra_interval_ms),
           resolve_interval(g_cfg.intervals.host_interval_ms,    g_cfg.infra_interval_ms),
           resolve_interval(g_cfg.intervals.gpu_interval_ms,     g_cfg.infra_interval_ms));

    while (g_running) {
        uint64_t now = now_us();

        /* Container metrics — require valid target_pid */
        if (g_cfg.target_pid > 0) {
            collect_container_cpu(now);
            collect_container_mem(now);
            collect_container_net(now);
            collect_container_disk(now);
            collect_container_proc(now);
            g_infra_shm->container.timestamp_us = now;
        }

        /* Host metrics */
        collect_host_metrics(now);

        /* GPU metrics */
        collect_gpu_metrics(now);

        /* Update global timestamp */
        g_infra_shm->timestamp_us = now;

        /* Sleep for one tick */
        usleep(tick_ms * 1000);
    }

    printf("AURORA_INFRA: Collector thread exiting\n");
    return NULL;
}

/* ===============================================================
 * PUBLIC API
 * =============================================================== */

int aurora_infra_init(const aurora_infra_config_t *cfg)
{
    if (!cfg) {
        fprintf(stderr, "AURORA_INFRA: NULL config\n");
        return -1;
    }
    if (!cfg->enabled) {
        printf("AURORA_INFRA: Disabled by configuration\n");
        return 0;
    }
    if (g_running) {
        fprintf(stderr, "AURORA_INFRA: Already running\n");
        return -1;
    }

    /* Copy config */
    memcpy(&g_cfg, cfg, sizeof(g_cfg));

    /* Default interval if not set */
    if (g_cfg.infra_interval_ms == 0)
        g_cfg.infra_interval_ms = 1000;

    /* Container discovery */
    if (g_cfg.target_pid == 0 && g_cfg.container_name[0] != '\0') {
        printf("AURORA_INFRA: Discovering container '%s'...\n", g_cfg.container_name);
        if (aurora_infra_discover_container(&g_cfg) == 0) {
            printf("AURORA_INFRA: Found container PID=%d cgroup=%s\n",
                   g_cfg.target_pid, g_cfg.cgroup_path);
        } else {
            fprintf(stderr, "AURORA_INFRA: Container discovery failed for '%s'\n",
                    g_cfg.container_name);
            /* Continue anyway — host metrics still work */
        }
    } else if (g_cfg.target_pid == 0 &&
               g_cfg.discover_mode == AURORA_CONTAINER_DISCOVER_AUTO) {
        printf("AURORA_INFRA: Auto-discovering RAN container...\n");
        if (aurora_infra_discover_container(&g_cfg) == 0) {
            printf("AURORA_INFRA: Auto-discovered container PID=%d cgroup=%s\n",
                   g_cfg.target_pid, g_cfg.cgroup_path);
        } else {
            fprintf(stderr, "AURORA_INFRA: Auto-discovery failed, "
                    "container metrics disabled\n");
        }
    }

    if (g_cfg.target_pid > 0) {
        /* Copy container name into SHM after discovery */
        printf("AURORA_INFRA: Monitoring container PID=%d name='%s'\n",
               g_cfg.target_pid, g_cfg.container_name);
    } else {
        printf("AURORA_INFRA: No target container — host-only mode\n");
    }

    /* eBPF backend init — only if requested (EBPF) or auto. */
    g_ebpf_active = 0;
#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_cfg.backend == AURORA_INFRA_BACKEND_EBPF ||
        g_cfg.backend == AURORA_INFRA_BACKEND_AUTO) {
        if (g_cfg.cgroup_path[0] != '\0') {
            if (aurora_infra_ebpf_init(&g_cfg) == 0) {
                g_ebpf_active = aurora_infra_ebpf_is_active();
                fprintf(stderr, "AURORA_INFRA: eBPF backend %s\n",
                        g_ebpf_active ? "ACTIVE" : "unavailable");
            } else if (g_cfg.backend == AURORA_INFRA_BACKEND_EBPF) {
                fprintf(stderr, "AURORA_INFRA: eBPF backend required but failed\n");
                return -1;
            } else {
                fprintf(stderr, "AURORA_INFRA: eBPF unavailable — procfs fallback\n");
            }
        } else {
            fprintf(stderr, "AURORA_INFRA: no cgroup_path — eBPF skipped\n");
        }
    }
#else
    if (g_cfg.backend == AURORA_INFRA_BACKEND_EBPF) {
        fprintf(stderr, "AURORA_INFRA: eBPF support not compiled in\n");
        return -1;
    }
#endif

    /* Create SHM segment */
    g_infra_shm = create_infra_shm(g_cfg.infra_shm_key);
    if (!g_infra_shm) {
        fprintf(stderr, "AURORA_INFRA: Failed to create SHM\n");
        return -1;
    }

    /* Copy container identity into SHM */
    snprintf(g_infra_shm->container.container_name,
             AURORA_INFRA_CONTAINER_NAME_LEN, "%s", g_cfg.container_name);
    g_infra_shm->container.container_pid = g_cfg.target_pid;

    /* Start collector thread */
    g_running = 1;
    if (pthread_create(&g_collector_tid, NULL, infra_collector_thread, NULL) != 0) {
        fprintf(stderr, "AURORA_INFRA: pthread_create failed: %s\n",
                strerror(errno));
        g_running = 0;
        return -1;
    }
    pthread_detach(g_collector_tid);

    printf("AURORA_INFRA: Initialized — SHM key=0x%x size=%zu bytes\n",
           g_cfg.infra_shm_key, sizeof(aurora_infra_metrics_t));
    return 0;
}

void aurora_infra_shutdown(void)
{
    if (!g_running) return;

    printf("AURORA_INFRA: Shutting down...\n");
    g_running = 0;

    /* Give collector thread time to exit */
    usleep(200000);  /* 200ms */

#ifdef AURORA_INFRA_EBPF_ENABLED
    if (g_ebpf_active) {
        aurora_infra_ebpf_shutdown();
        g_ebpf_active = 0;
    }
#endif

    if (g_infra_shm) {
        shmdt(g_infra_shm);
        g_infra_shm = NULL;
        printf("AURORA_INFRA: Detached from SHM\n");
    }
    if (g_infra_shmid >= 0) {
        if (shmctl(g_infra_shmid, IPC_RMID, NULL) == 0)
            printf("AURORA_INFRA: Removed SHM segment\n");
        else
            perror("AURORA_INFRA: shmctl IPC_RMID failed");
        g_infra_shmid = -1;
    }
}

aurora_infra_metrics_t *aurora_infra_get_metrics(key_t shm_key)
{
    int shmid = shmget(shm_key, sizeof(aurora_infra_metrics_t), 0666);
    if (shmid < 0) {
        /* Try with size 0 to attach to existing segment */
        shmid = shmget(shm_key, 0, 0666);
        if (shmid < 0) return NULL;
    }
    void *ptr = shmat(shmid, NULL, SHM_RDONLY);
    if (ptr == (void *)-1) return NULL;
    return (aurora_infra_metrics_t *)ptr;
}
