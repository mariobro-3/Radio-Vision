# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED BOARD)
    if(DEFINED ENV{CSI_BOARD})
        set(BOARD "$ENV{CSI_BOARD}")
    else()
        set(BOARD "feather_s3")
    endif()
endif()

if(BOARD STREQUAL "feather_s3")
    set(IDF_TARGET "esp32s3")
    list(APPEND EXTRA_SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../boards/feather_s3/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(active-client)