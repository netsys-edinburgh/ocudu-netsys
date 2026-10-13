# Standalone build of aurora_reader (AURORA SHM reader + infra sidecar collector) without the rest of OCUDU.
# Used by docker/aurora_reader/Dockerfile:
#   cmake -S docker/aurora_reader -B build && cmake --build build && cmake --install build
cmake_minimum_required(VERSION 3.14)
project(aurora_reader_sidecar C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OCUDU_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Threads REQUIRED)
option(ENABLE_AURORA_EBPF "Enable eBPF backends of the AURORA metrics framework" ON)
include_directories(${OCUDU_ROOT}/include)

# Only the libraries aurora_reader links are built (EXCLUDE_FROM_ALL keeps the in-process core out).
add_subdirectory(${OCUDU_ROOT}/lib/aurora lib/aurora EXCLUDE_FROM_ALL)
add_subdirectory(${OCUDU_ROOT}/utils/aurora_reader utils/aurora_reader)
