# ESP-IDF Project CMakeLists.txt for Eldra-v2
# Waveshare ESP32-S3 2.8" Round Display

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(eldra_v2)
