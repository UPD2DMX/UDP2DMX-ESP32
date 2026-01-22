# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

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
