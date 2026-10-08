#
# AURORA RAN metrics framework + ANOMALY injection (port of OAI openair2/AURORA and openair2/ANOMALY).
#
# The thread/infra collectors and the anomaly server are plain C (kept close to the OAI sources);
# the core (hooks, publisher, lifecycle) is C++.
#

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(LIBBPF QUIET libbpf)
endif ()
find_program(AURORA_CLANG_BPF clang DOC "clang for BPF compilation")
find_program(AURORA_BPFTOOL bpftool PATHS /usr/sbin /sbin DOC "bpftool for BPF skeleton generation")

set(AURORA_EBPF_AVAILABLE OFF)
if (ENABLE_AURORA_EBPF AND LIBBPF_FOUND AND AURORA_CLANG_BPF AND AURORA_BPFTOOL)
    set(AURORA_EBPF_AVAILABLE ON)
    message(STATUS "AURORA: libbpf ${LIBBPF_VERSION}, clang and bpftool found - eBPF backends enabled")
else ()
    message(STATUS "AURORA: eBPF backends disabled (procfs only)")
endif ()

# Compiles <name>.bpf.c into an object and generates <name>.skel.h with bpftool.
function(aurora_bpf_skeleton name src_dir out_target)
    set(bpf_obj ${CMAKE_CURRENT_BINARY_DIR}/${name}.bpf.o)
    set(bpf_skel ${CMAKE_CURRENT_BINARY_DIR}/${name}.skel.h)
    add_custom_command(
            OUTPUT ${bpf_obj}
            COMMAND ${AURORA_CLANG_BPF} -target bpf -O2 -g -D__TARGET_ARCH_x86
            -I${src_dir} -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_BINARY_DIR}
            -c ${src_dir}/${name}.bpf.c -o ${bpf_obj}
            DEPENDS ${src_dir}/${name}.bpf.c ${src_dir}/${name}.h ${CMAKE_CURRENT_SOURCE_DIR}/vmlinux.h
            COMMENT "Compiling BPF program ${name}.bpf.c"
            VERBATIM)
    add_custom_command(
            OUTPUT ${bpf_skel}
            COMMAND ${AURORA_BPFTOOL} gen skeleton ${bpf_obj} > ${bpf_skel}
            DEPENDS ${bpf_obj}
            COMMENT "Generating BPF skeleton ${name}.skel.h")
    add_custom_target(${out_target} DEPENDS ${bpf_skel})
endfunction()

# --- Per-thread runtime metrics (Group 17) ---
set(AURORA_THREAD_SOURCES
        thread/aurora_thread.c
        thread/aurora_thread_stats.c
        thread/aurora_thread_registry.c
        thread/aurora_thread_proc.c)
if (AURORA_EBPF_AVAILABLE)
    aurora_bpf_skeleton(aurora_thread_ebpf ${CMAKE_CURRENT_SOURCE_DIR}/thread aurora_thread_bpf_skel)
    list(APPEND AURORA_THREAD_SOURCES thread/aurora_thread_ebpf.c)
endif ()
add_library(ocudu_aurora_thread STATIC ${AURORA_THREAD_SOURCES})
target_include_directories(ocudu_aurora_thread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/thread)
target_link_libraries(ocudu_aurora_thread PUBLIC m Threads::Threads)
if (AURORA_EBPF_AVAILABLE)
    add_dependencies(ocudu_aurora_thread aurora_thread_bpf_skel)
    target_compile_definitions(ocudu_aurora_thread PUBLIC AURORA_THREAD_EBPF_ENABLED=1)
    target_include_directories(ocudu_aurora_thread PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${LIBBPF_INCLUDE_DIRS})
    target_link_libraries(ocudu_aurora_thread PUBLIC ${LIBBPF_LIBRARIES})
endif ()

# --- Infrastructure / container metrics (Group 18), used by aurora_reader ---
set(AURORA_INFRA_SOURCES
        infra/aurora_infra.c
        infra/aurora_infra_config.c
        infra/aurora_infra_procfs.c)
if (AURORA_EBPF_AVAILABLE)
    aurora_bpf_skeleton(aurora_infra_ebpf ${CMAKE_CURRENT_SOURCE_DIR}/infra aurora_infra_bpf_skel)
    list(APPEND AURORA_INFRA_SOURCES infra/aurora_infra_ebpf.c)
endif ()
add_library(ocudu_aurora_infra STATIC ${AURORA_INFRA_SOURCES})
target_include_directories(ocudu_aurora_infra PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/infra)
target_link_libraries(ocudu_aurora_infra PUBLIC m Threads::Threads)
if (AURORA_EBPF_AVAILABLE)
    add_dependencies(ocudu_aurora_infra aurora_infra_bpf_skel)
    target_compile_definitions(ocudu_aurora_infra PUBLIC AURORA_INFRA_EBPF_ENABLED=1)
    target_include_directories(ocudu_aurora_infra PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${LIBBPF_INCLUDE_DIRS})
    target_link_libraries(ocudu_aurora_infra PUBLIC ${LIBBPF_LIBRARIES})
endif ()

# --- ANOMALY injection server ---
add_library(ocudu_aurora_anomaly STATIC anomaly/anomaly_module.c)
target_include_directories(ocudu_aurora_anomaly PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/anomaly)
target_link_libraries(ocudu_aurora_anomaly PUBLIC Threads::Threads)

# --- Shared C helpers (also linked by aurora_reader) ---
add_library(ocudu_aurora_common STATIC aurora_common.c)
target_include_directories(ocudu_aurora_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocudu_aurora_common PUBLIC ocudu_aurora_thread)

# --- Core: hooks, publisher, lifecycle ---
add_library(ocudu_aurora STATIC aurora_hooks.cpp aurora_publish.cpp aurora_lifecycle.cpp)
target_include_directories(ocudu_aurora PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocudu_aurora PUBLIC ocudu_aurora_common ocudu_aurora_anomaly Threads::Threads)
