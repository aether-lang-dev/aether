# ESP-IDF project: two FreeRTOS tasks entering one cooperative Aether runtime,
# each with a task context of its own (docs/design/task-execution-contexts.md).
# The library is tests/integration/coop_task_contexts/ctxlib.ae, its C in
# /gen; the host is main/host_esp.c. Driven by tests/esp32/test_esp32_contexts.sh.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aether_esp32_contexts)
