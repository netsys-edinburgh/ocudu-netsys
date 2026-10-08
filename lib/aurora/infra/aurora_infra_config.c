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
 * aurora_infra_config.c — Configuration parsing for infrastructure metrics.
 *
 * Entry points (OCUDU port: infra is configured only via aurora_reader CLI):
 *   1. aurora_infra_default_config() — defaults
 *   2. aurora_infra_parse_cli()      — parse --infra-* aurora_reader CLI args
 */

#include "aurora_infra.h"
#include <stdlib.h>

/* Defaults (were aurora_paramdef.h in OAI; OCUDU has no libconfig section for infra). */
static const char* const aurora_config_infra_shm_key_default          = "0xAEF10001";
static const uint32_t    aurora_config_infra_interval_ms_default      = 1000;
static const char* const aurora_config_infra_host_proc_prefix_default = "/host";
static const char* const aurora_config_infra_host_sys_prefix_default  = "/host";
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int str_is_yes(const char *s)
{
    return (strcmp(s, "yes") == 0 || strcmp(s, "1") == 0 ||
            strcmp(s, "true") == 0 || strcmp(s, "on") == 0);
}

/* ---------------------------------------------------------------
 * Parse / override from CLI arguments (used by aurora_reader).
 *
 * Scans argv for --infra-* flags and updates cfg in-place.
 * Returns number of argv entries consumed (for caller to skip).
 *
 * Recognized flags:
 *   --infra-enabled yes|no
 *   --infra-shm-key <hex>
 *   --infra-interval <ms>
 *   --infra-container <name>
 *   --infra-discover <manual|auto>
 *   --infra-gpu yes|no
 *   --infra-host-proc <prefix>
 *   --infra-host-sys <prefix>
 *   --infra-cpu-interval <ms>
 *   --infra-mem-interval <ms>
 *   --infra-net-interval <ms>
 *   --infra-disk-interval <ms>
 *   --infra-host-interval <ms>
 *   --infra-gpu-interval <ms>
 * --------------------------------------------------------------- */
int aurora_infra_parse_cli(aurora_infra_config_t *cfg, int argc, char **argv)
{
    int consumed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--infra-enabled") && i + 1 < argc) {
            cfg->enabled = str_is_yes(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-shm-key") && i + 1 < argc) {
            cfg->infra_shm_key = (key_t)strtol(argv[++i], NULL, 0);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-interval") && i + 1 < argc) {
            cfg->infra_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-container") && i + 1 < argc) {
            i++;
            strncpy(cfg->container_name, argv[i], AURORA_INFRA_CONTAINER_NAME_LEN - 1);
            cfg->container_name[AURORA_INFRA_CONTAINER_NAME_LEN - 1] = '\0';
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-discover") && i + 1 < argc) {
            i++;
            cfg->discover_mode = (strcmp(argv[i], "manual") == 0)
                ? AURORA_CONTAINER_DISCOVER_MANUAL
                : AURORA_CONTAINER_DISCOVER_AUTO;
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-gpu") && i + 1 < argc) {
            cfg->gpu_enabled = str_is_yes(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-host-proc") && i + 1 < argc) {
            i++;
            strncpy(cfg->host_proc_prefix, argv[i], AURORA_INFRA_PATH_LEN - 1);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-host-sys") && i + 1 < argc) {
            i++;
            strncpy(cfg->host_sys_prefix, argv[i], AURORA_INFRA_PATH_LEN - 1);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-cpu-interval") && i + 1 < argc) {
            cfg->intervals.cpu_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-mem-interval") && i + 1 < argc) {
            cfg->intervals.memory_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-net-interval") && i + 1 < argc) {
            cfg->intervals.network_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-disk-interval") && i + 1 < argc) {
            cfg->intervals.disk_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-host-interval") && i + 1 < argc) {
            cfg->intervals.host_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-gpu-interval") && i + 1 < argc) {
            cfg->intervals.gpu_interval_ms = (uint32_t)atoi(argv[++i]);
            consumed += 2;
        } else if (!strcmp(argv[i], "--infra-backend") && i + 1 < argc) {
            const char *b = argv[++i];
            if      (!strcmp(b, "ebpf"))   cfg->backend = AURORA_INFRA_BACKEND_EBPF;
            else if (!strcmp(b, "procfs")) cfg->backend = AURORA_INFRA_BACKEND_PROCFS;
            else                           cfg->backend = AURORA_INFRA_BACKEND_AUTO;
            consumed += 2;
        }
    }
    return consumed;
}

/* ---------------------------------------------------------------
 * Defaults — return config with sane defaults (for standalone use)
 * --------------------------------------------------------------- */
aurora_infra_config_t aurora_infra_default_config(void)
{
    aurora_infra_config_t cfg = {0};
    cfg.enabled = 0;
    cfg.backend = AURORA_INFRA_BACKEND_AUTO;
    cfg.discover_mode = AURORA_CONTAINER_DISCOVER_AUTO;
    cfg.infra_shm_key = (key_t)strtol(aurora_config_infra_shm_key_default, NULL, 0);
    cfg.infra_interval_ms = aurora_config_infra_interval_ms_default;
    strncpy(cfg.host_proc_prefix, aurora_config_infra_host_proc_prefix_default,
            AURORA_INFRA_PATH_LEN - 1);
    strncpy(cfg.host_sys_prefix, aurora_config_infra_host_sys_prefix_default,
            AURORA_INFRA_PATH_LEN - 1);
    return cfg;
}
