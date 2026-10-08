/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * aurora_infra_ebpf.h — shared between BPF program and userspace loader.
 */

#ifndef AURORA_INFRA_EBPF_H
#define AURORA_INFRA_EBPF_H

/* __u32/__u64 come from vmlinux.h in BPF context, linux/types.h otherwise. */
#if !defined(__VMLINUX_H__) && !defined(__bpf__)
#include <linux/types.h>
#endif

struct aurora_infra_cg_stats {
    __u64 vol_ctxsw;
    __u64 invol_ctxsw;
    __u64 pgfault_user;
    __u64 tcp_retrans;
    __u64 block_ios;
    __u64 runtime_ns;
};

#ifndef __bpf__
#include "aurora_infra.h"

int  aurora_infra_ebpf_init(const aurora_infra_config_t *cfg);
void aurora_infra_ebpf_shutdown(void);
int  aurora_infra_ebpf_is_active(void);
int  aurora_infra_ebpf_collect_container_cpu(aurora_container_cpu_t *out);
int  aurora_infra_ebpf_collect_container_mem(aurora_container_mem_t *out);
int  aurora_infra_ebpf_collect_container_net(aurora_container_net_t *out);
int  aurora_infra_ebpf_collect_container_io (aurora_container_io_t  *out);

/*
 * aurora_infra_ebpf_collect_host_nic_carrier — read the eBPF-tracked
 * link up/down transition count for a given host NIC by name.
 *
 * Host-wide (not cgroup-filtered) — netif_carrier_on/off run in whatever
 * context the driver/link-watch triggers them from, not the monitored
 * container's task context, so target_cgid filtering does not apply here.
 *
 * Unlike the procfs carrier_changes read (lifetime-since-boot, from
 * /sys/class/net/<iface>/carrier_changes), this counts transitions only
 * since the BPF program attached — it will read lower than the procfs
 * value until a full link-flap cycle has occurred after attach. The two
 * are not meant to be summed; aurora_infra.c's merge policy supersedes
 * the procfs value with this one when eBPF is active, same as the other
 * eBPF-vs-procfs fields.
 *
 * Returns 0 and writes *out on success (including 0 if no flaps seen),
 * -1 if eBPF is inactive or the interface name does not resolve.
 */
int aurora_infra_ebpf_collect_host_nic_carrier(const char *iface_name, uint64_t *out);
#endif

#endif /* AURORA_INFRA_EBPF_H */
