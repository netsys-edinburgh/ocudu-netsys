set(SOURCES
        aurora_config_cli11_schema.cpp
        aurora_config_yaml_writer.cpp
        aurora_service.cpp)

add_library(ocudu_aurora_app_service STATIC ${SOURCES})
target_include_directories(ocudu_aurora_app_service PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_aurora_app_service PUBLIC ocudu_aurora PRIVATE ocudulog ocudu::yaml-cpp)
