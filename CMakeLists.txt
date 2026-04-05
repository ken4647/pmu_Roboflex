cmake_minimum_required(VERSION 3.10)
project(pmu_roboflex C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
)

find_package(Threads REQUIRED)

# Prefer vendored cJSON submodule if present, otherwise fall back to system lib
set(HAS_CJSON_SUBMODULE OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON/CMakeLists.txt)
    set(HAS_CJSON_SUBMODULE ON)
    add_subdirectory(third_party/cJSON)
endif()

if(NOT HAS_CJSON_SUBMODULE)
    find_library(CJSON_LIB cjson REQUIRED)
endif()
find_library(CAP_LIB cap REQUIRED)
find_library(DL_LIB dl REQUIRED)

add_executable(robonix_daemon
    src/server.c
)

if(HAS_CJSON_SUBMODULE)
    target_link_libraries(robonix_daemon
        cjson
        ${CAP_LIB}
    )
else()
    target_link_libraries(robonix_daemon
        ${CJSON_LIB}
        ${CAP_LIB}
    )
endif()

add_library(robonix SHARED
    src/dym_hook.c
    src/robflex_api.c
    src/robflex_config.c
)

if(HAS_CJSON_SUBMODULE)
    target_link_libraries(robonix
        Threads::Threads
        ${DL_LIB}
        cjson
    )
else()
    target_link_libraries(robonix
        Threads::Threads
        ${DL_LIB}
        ${CJSON_LIB}
    )
endif()

add_executable(proc_stat
    src/proc_stat.c
)

target_link_libraries(proc_stat
    rt
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

