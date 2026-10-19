#!/bin/bash

#
# This script installs the runtime dependencies of the AURORA anomaly module (lib/aurora/anomaly): stress-ng, which
# MAC_CONTENTION and PDCP_CONTENTION run on the CPUs of the dedicated MAC and PDCP workers.
#
# Run like this: ./install_aurora_anomaly_dependencies.sh
#
# The anomaly module only reports an error for those two anomaly types when stress-ng is missing, so this script never
# fails the image build because of it, and it does nothing on distributions other than Debian and Ubuntu.
#

set -e

main() {
    . /etc/os-release
    case "$ID" in
        ubuntu | debian)
            local -x DEBIAN_FRONTEND=noninteractive
            apt-get update
            if apt-cache policy stress-ng 2>/dev/null | grep -q 'Candidate: [^(]'; then
                apt-get install -y --no-install-recommends stress-ng
            else
                echo "AURORA anomaly: package stress-ng not available, skipping"
            fi
            apt-get clean && rm -rf /var/lib/apt/lists/*
            ;;
        *)
            echo "AURORA anomaly: $ID not supported, stress-ng not installed"
            ;;
    esac
}

main "$@"
