# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Board/target autodetect (defaults to FeatherS3)
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

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(udp_client)
