# Host-based test project (aggregator_DESIGN.md §9) — builds and runs
# natively on Linux via `idf.py --preview set-target linux`, no ESP32
# hardware or cross toolchain required.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(host_test_aggregator)
