# Music Reactive LED Strip firmware - top level ESP-IDF project file.
#
# ESP-IDF is used only for: build system, FreeRTOS, WiFi/TCP-IP stack, 2nd stage
# bootloader, OTA partition handling and flash access. All peripheral drivers
# (I2S microphone, WS2812 strip, watchdog) and all DSP code are our own (Interface/).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/App"
    "${CMAKE_CURRENT_LIST_DIR}/Interface")

# Only build App and what it (transitively) requires -> smaller image.
set(COMPONENTS App)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# build_config.h lives in App/ but is needed by every module.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/App" APPEND)

# Release build (idf.py -DLEDFW_RELEASE=1 ...): build_config.h then leaves
# INTERNAL_TEST_BUILD (debug CLI + self diagnostics) undefined.
if(LEDFW_RELEASE)
    idf_build_set_property(COMPILE_DEFINITIONS "LEDFW_RELEASE_BUILD" APPEND)
endif()

project(led_strip_fw)
