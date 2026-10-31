cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/../../Buildscripts/module.cmake")

file(GLOB_RECURSE SOURCE_FILES "source/*.c*")

if (DEFINED ENV{ESP_IDF_VERSION})
    set(JSON_REQUIRES json)
else ()
    set(JSON_REQUIRES cJSON)
endif ()

tactility_add_module(agents
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS include/
    REQUIRES Tactility
    PRIV_REQUIRES ${JSON_REQUIRES}
)
