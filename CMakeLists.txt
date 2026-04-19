# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# -------------------- Versioning --------------------
# Version format: MAJOR.MINOR.BUILD[-dirty]
# - MAJOR/MINOR: set manually here
# - BUILD: auto derived from git commit count (increments by 1 per commit)
set(UDP2DMX_VERSION_MAJOR 0)
set(UDP2DMX_VERSION_MINOR 6)

set(UDP2DMX_BUILD_NUMBER 0)
set(UDP2DMX_DIRTY_SUFFIX "")

# Best-effort: derive build number from git if available.
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE UDP2DMX_BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE UDP2DMX_GIT_RESULT
)

if(NOT UDP2DMX_GIT_RESULT EQUAL 0 OR "${UDP2DMX_BUILD_NUMBER}" STREQUAL "")
    set(UDP2DMX_BUILD_NUMBER 0)
endif()

# Best-effort: mark dirty working tree.
execute_process(
    COMMAND git diff --quiet
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ERROR_QUIET
    RESULT_VARIABLE UDP2DMX_GIT_DIRTY_RESULT
)
if(NOT UDP2DMX_GIT_DIRTY_RESULT EQUAL 0)
    set(UDP2DMX_DIRTY_SUFFIX "-dirty")
endif()

set(PROJECT_VER "${UDP2DMX_VERSION_MAJOR}.${UDP2DMX_VERSION_MINOR}.${UDP2DMX_BUILD_NUMBER}${UDP2DMX_DIRTY_SUFFIX}")

# Allow specifying different sdkconfig files based on chip variant
# Usage: idf.py -DCHIP_VARIANT=4mb build
if(DEFINED CHIP_VARIANT)
    set(SDKCONFIG_DEFAULT "sdkconfig.default.${CHIP_VARIANT}")
    message(STATUS "Using chip variant: ${CHIP_VARIANT} (${SDKCONFIG_DEFAULT})")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# include($ENV{IDF_PATH}/tools/cmake/spiffs.cmake)
set(EXTRA_COMPONENT_DIRS components)
project(UDP2DMX)
