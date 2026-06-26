#!/bin/bash

# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set -e # stop executing after error

apply_dpdk_patches() {
    local dpdk_version=$1
    local script_dir=$2
    local patch_dir="${DPDK_PATCH_DIR:-${script_dir}/../patches/dpdk/${dpdk_version}}"

    [ ! -d "$patch_dir" ] && return

    shopt -s nullglob
    local patch_file
    for patch_file in "$patch_dir"/*.patch; do
        echo "Applying DPDK patch: ${patch_file}"
        patch -p1 < "$patch_file"
    done
    shopt -u nullglob
}

main() {
    # Check number of args
    if [ $# -lt 1 ] || [ $# -gt 3 ]; then
        echo >&2 "Illegal number of parameters"
        echo >&2 "Run like this: \"./build_dpdk.sh <dpdk_version> [<arch> [<ncores>]]\" where arch is any gcc/clang compatible -march and ncores could be any number or empty for all"
        exit 1
    fi

    local dpdk_version=$1
    local arch="${2:--Dcpu_instruction_set=native}"
    local ncores="${3:-$(nproc)}"
    local script_dir

    script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

    cd /tmp
    curl -L "https://fast.dpdk.org/rel/dpdk-${dpdk_version}.tar.xz" | tar xJf -
    cd dpdk*"${dpdk_version}"
    apply_dpdk_patches "${dpdk_version}" "${script_dir}"
    read -ra arch_args <<< "${arch}"
    meson setup build --prefix "/opt/dpdk/${dpdk_version}" "${arch_args[@]}"
    meson compile -j "${ncores}" -C build
    meson install -C build

    rm -Rf /tmp/dpdk*"${dpdk_version}"
    rm -Rf /opt/dpdk/"${dpdk_version}"/bin/*  # Remove binaries due to their size ~250MB
}

main "$@"
