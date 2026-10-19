/*
 * Application anomaly control module for OAI RAN processes.
 *
 * The module exposes a small TCP command interface and lightweight hook APIs
 * used from MAC/RLC/PDCP update paths.
 */

#ifndef OAI_ANOMALY_MODULE_H
#define OAI_ANOMALY_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int ran_type;
  uint32_t listen_port;
  const char *bind_address;
} anomaly_module_config_t;

/* Start the anomaly control listener. Safe to call multiple times. */
int anomaly_module_start(const anomaly_module_config_t *cfg);

/* Stop listener and cleanup all anomaly state/resources. */
void anomaly_module_stop(void);

/* Runtime hooks from RAN update paths. */
void anomaly_module_hook_mac(void);
void anomaly_module_hook_rlc(void);
void anomaly_module_hook_pdcp(void);

/* Anomaly label last set with "LABEL value=..." ("normal" until then), copied NUL-terminated into @out. The label is
 * set by the anomaly-injector sidecar only; SET / CLEAR do not change it. Called once per AURORA collection interval
 * by the publisher. */
#define ANOMALY_LABEL_LEN 32
void anomaly_module_get_label(char *out, size_t out_sz);

/* Helpers for wiring default role-specific listener ports. */
uint32_t anomaly_module_default_port_for_ran(int ran_type);
const char *anomaly_module_role_name(int ran_type);

#ifdef __cplusplus
}
#endif

#endif /* OAI_ANOMALY_MODULE_H */
