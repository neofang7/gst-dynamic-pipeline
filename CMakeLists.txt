# Copyright (c) 2020 Intel Corporation.
# All rights reserved.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required (VERSION 2.8.12)
include(FindPkgConfig)

project(zoom)
set (PROGRAM_NAME zoom)

add_compile_options(-g -O0 -Wall -fPIE -D_FORTIFY_SOURCE=2)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread" )
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,now -Wl,-z,relro")

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

add_executable(${PROGRAM_NAME} main.cpp)
target_include_directories(${PROGRAM_NAME} PRIVATE ./)
target_include_directories(${PROGRAM_NAME} PRIVATE /usr/include/)
target_include_directories(${PROGRAM_NAME} PRIVATE /usr/include/gstreamer-1.0/)
target_include_directories(${PROGRAM_NAME} PRIVATE /usr/include/glib-2.0/)
target_include_directories(${PROGRAM_NAME} PRIVATE /usr/lib/x86_64-linux-gnu/glib-2.0/include/)
target_link_libraries(${PROGRAM_NAME} PRIVATE gstreamer-1.0)
target_link_libraries(${PROGRAM_NAME} PRIVATE gobject-2.0)
target_link_libraries(${PROGRAM_NAME} PRIVATE glib-2.0)

