# aurora_reader: attaches to an AURORA shared-memory segment and exports one CSV row per collection interval
# (port of OAI openair2/AURORA/aurora_reader.c). Optional infra sidecar collector (--infra-*).
add_executable(aurora_reader aurora_reader.c aurora_reader_influx.c)
target_link_libraries(aurora_reader ocudu_aurora_common ocudu_aurora_infra m)
install(TARGETS aurora_reader RUNTIME)
