# ESP-IDF project that builds one generated Aether program (-DAE_PROG=<name>,
# its C in /gen) with the Aether runtime, for the ESP32 (Xtensa). Driven by
# tests/esp32/test_esp32.sh; see that file.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aether_esp32)
