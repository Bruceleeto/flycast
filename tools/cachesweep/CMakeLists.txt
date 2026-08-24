# Standalone: built separately from flycast so that it stays a small, fast
# command line tool and cannot break the emulator build.
#
#   cmake -S tools/cachesweep -B build-cachesweep && cmake --build build-cachesweep
cmake_minimum_required(VERSION 3.16)
project(cachesweep CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(cachesweep cachesweep.cpp)
# The model header is shared with flycast on purpose: one implementation, so a
# sweep cannot optimise against a model that has drifted from the measurement.
target_include_directories(cachesweep PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/hw/sh4/cachesim)
