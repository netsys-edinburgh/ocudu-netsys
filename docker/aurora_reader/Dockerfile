#
# aurora_reader sidecar: reads the AURORA shared-memory segment of an OCUDU RAN container (gnb / du / cu / cu_cp /
# cu_up) and writes one CSV row per collection interval. Optionally samples host and container metrics (infra).
#
# Build (from the repository root):
#   docker build -f docker/aurora_reader/Dockerfile -t ocudu/aurora_reader .
#
# Run next to a RAN container (see docker/aurora_reader/docker-compose.aurora.yml), at minimum:
#   docker run --rm --ipc container:ocudu_gnb -v $PWD/aurora-data:/data ocudu/aurora_reader
#
# Debian bookworm provides libbpf >= 1.0 (required to load BPF programs on kernels >= 6.0), bpftool and clang, so
# the eBPF infra backend is built without extra steps.

ARG OS_VERSION=bookworm
ARG DOCKER_CLI_VERSION=27.3.1

################
# Builder      #
################
FROM debian:${OS_VERSION} AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config clang llvm bpftool libbpf-dev libelf-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Only the sources aurora_reader needs.
COPY include/ocudu/aurora/ /src/include/ocudu/aurora/
COPY lib/aurora/           /src/lib/aurora/
COPY utils/aurora_reader/  /src/utils/aurora_reader/
COPY docker/aurora_reader/ /src/docker/aurora_reader/

RUN cmake -S /src/docker/aurora_reader -B /build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/aurora \
    && cmake --build /build -j"$(nproc)" \
    && cmake --install /build \
    && strip /opt/aurora/bin/aurora_reader

# Static docker CLI: container auto-discovery (`docker ps` / `docker inspect` on the mounted docker.sock).
ARG DOCKER_CLI_VERSION
ARG TARGETARCH
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && case "${TARGETARCH:-amd64}" in arm64) a=aarch64 ;; *) a=x86_64 ;; esac \
    && curl -fsSL "https://download.docker.com/linux/static/stable/${a}/docker-${DOCKER_CLI_VERSION}.tgz" \
       | tar -xz -C /tmp docker/docker \
    && install -m 0755 /tmp/docker/docker /opt/aurora/bin/docker

################
# Runtime      #
################
FROM debian:${OS_VERSION}-slim

RUN apt-get update && apt-get install -y --no-install-recommends libbpf1 libelf1 zlib1g procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/aurora/bin/ /usr/local/bin/
COPY docker/aurora_reader/entrypoint.sh /usr/local/bin/aurora-sidecar

VOLUME /data
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/aurora-sidecar"]
