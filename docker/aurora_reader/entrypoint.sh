#!/bin/sh
# aurora_reader sidecar entrypoint.
#
# Arguments, if any, are passed to aurora_reader unchanged (e.g. `--once`). Otherwise the command line is built from:
#   AURORA_CONFIG          OCUDU app YAML with an `aurora:` section (shm_key / collection_interval_ms / ran_type)
#   AURORA_SHM_KEY         SysV key of the RAN process segment            (default 0xDEADBEEF)
#   AURORA_RAN_TYPE        gnb|du|cu|cu_cp|cu_up; empty = read from SHM   (default empty)
#   AURORA_INTERVAL_MS     poll interval                                   (default: from config / 100)
#   AURORA_SAMPLES         rows buffered per CSV flush                     (default 100)
#   AURORA_OUTPUT          CSV path; the RAN type is prefixed to the name  (default /data/aurora_metrics_<ts>.csv)
#   AURORA_INFRA           yes|no host/container metrics                   (default yes)
#   AURORA_INFRA_BACKEND   auto|ebpf|procfs                                (default auto)
#   AURORA_INFRA_CONTAINER RAN container name; empty = auto by RAN type    (default empty)
#   AURORA_INFRA_INTERVAL  infra poll interval, ms                         (default 1000)
#   AURORA_HOST_PREFIX     where the host /proc and /sys are mounted       (default /host)
#   AURORA_WAIT_S          seconds to wait for the RAN segment, 0 = forever (default 0)
set -eu

if [ "$#" -gt 0 ]; then
    exec aurora_reader "$@"
fi

set --
[ -n "${AURORA_CONFIG:-}" ] && set -- "$@" "${AURORA_CONFIG}"
[ -n "${AURORA_SHM_KEY:-}" ] && set -- "$@" --shm-key "${AURORA_SHM_KEY}"
[ -n "${AURORA_RAN_TYPE:-}" ] && set -- "$@" --ran-type "${AURORA_RAN_TYPE}"
[ -n "${AURORA_INTERVAL_MS:-}" ] && set -- "$@" --interval "${AURORA_INTERVAL_MS}"
set -- "$@" --samples "${AURORA_SAMPLES:-100}"
[ -n "${AURORA_OUTPUT:-}" ] && set -- "$@" --output "${AURORA_OUTPUT}"

if [ "${AURORA_INFRA:-yes}" = "yes" ]; then
    set -- "$@" --infra-enabled yes \
        --infra-backend "${AURORA_INFRA_BACKEND:-auto}" \
        --infra-interval "${AURORA_INFRA_INTERVAL:-1000}" \
        --infra-host-proc "${AURORA_HOST_PREFIX:-/host}" \
        --infra-host-sys "${AURORA_HOST_PREFIX:-/host}"
    [ -n "${AURORA_INFRA_CONTAINER:-}" ] && set -- "$@" --infra-container "${AURORA_INFRA_CONTAINER}"
fi

# Wait until the RAN process has created its AURORA segment (it may start after the sidecar).
key="${AURORA_SHM_KEY:-0xDEADBEEF}"
waited=0
until aurora_reader --shm-key "$key" --once >/dev/null 2>&1; do
    if [ "${AURORA_WAIT_S:-0}" -gt 0 ] && [ "$waited" -ge "${AURORA_WAIT_S}" ]; then
        echo "aurora_reader sidecar: no AURORA segment with key $key after ${waited}s" >&2
        exit 1
    fi
    [ "$waited" -eq 0 ] && echo "aurora_reader sidecar: waiting for AURORA segment key $key ..." >&2
    sleep 1
    waited=$((waited + 1))
done

cd /data
echo "aurora_reader sidecar: aurora_reader $*" >&2
exec aurora_reader "$@"
