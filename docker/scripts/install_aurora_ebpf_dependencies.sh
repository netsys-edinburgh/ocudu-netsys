#!/bin/bash

#
# This script installs the dependencies of the AURORA eBPF backends (lib/aurora).
#
# Run like this: ./install_aurora_ebpf_dependencies.sh <mode>
# E.g.: ./install_aurora_ebpf_dependencies.sh build
# E.g.: ./install_aurora_ebpf_dependencies.sh run
#
# The eBPF backends are optional: when their dependencies are missing, CMake builds AURORA with the procfs backends
# only. So this script never fails the image build because of a missing package. It installs whatever the
# distribution provides, and it does nothing on distributions other than Debian and Ubuntu.
#

set -e

install_aurora_ebpf_dependencies_debian_ubuntu() {
    local mode="${1:?}"
    local -x DEBIAN_FRONTEND=noninteractive
    local -a wanted=()
    local -a pkgs=()
    local pkg

    case "$mode" in
        build)
            # clang and llvm compile the BPF programs, bpftool generates their skeletons, libbpf loads them.
            wanted=(clang llvm bpftool libbpf-dev libelf-dev zlib1g-dev)
            ;;
        run)
            wanted=(libbpf1)
            ;;
        *)
            echo >&2 "Unsupported mode: $mode"
            exit 1
            ;;
    esac

    apt-get update
    for pkg in "${wanted[@]}"; do
        # Only packages with an installation candidate (e.g. Ubuntu 22.04 has no libbpf1 nor a standalone bpftool).
        if apt-cache policy "$pkg" 2>/dev/null | grep -q 'Candidate: [^(]'; then
            pkgs+=("$pkg")
        else
            echo "AURORA eBPF: package $pkg not available, skipping"
        fi
    done

    if ((${#pkgs[@]})); then
        apt-get install -y --no-install-recommends "${pkgs[@]}"
    fi
    apt-get autoremove -y && apt-get clean && rm -rf /var/lib/apt/lists/*
}

main() {
    if [ $# != 1 ]; then
        echo >&2 "Illegal number of parameters"
        echo >&2 "Run like this: \"./install_aurora_ebpf_dependencies.sh <mode>\" where mode could be: build or run"
        exit 1
    fi

    # shellcheck source=/dev/null
    . /etc/os-release

    echo "== Installing AURORA eBPF dependencies, mode $1 =="

    case "$ID" in
        debian|ubuntu)
            install_aurora_ebpf_dependencies_debian_ubuntu "$1"
            ;;
        *)
            echo "AURORA eBPF: OS $ID not handled, AURORA will use its procfs backends"
            ;;
    esac
}

main "$@"
