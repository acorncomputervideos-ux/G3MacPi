# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host (Linux) build: compiles the portable emulator core and runs unit tests.
# The bare-metal Raspberry Pi build uses Circle's make system instead
# (see app/circle/Makefile and tools/build-pi.sh).

cmake_minimum_required(VERSION 3.16)
project(G3MacPi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(G3_WERROR "Treat warnings as errors" ON)
option(G3_SANITIZE "Build tests with AddressSanitizer + UBSan" OFF)

# Flags shared by core and tests. Exceptions/RTTI are disabled to match the
# Circle build, so incompatible code is caught on the host first.
set(G3_COMMON_FLAGS -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti)
if(G3_WERROR)
  list(APPEND G3_COMMON_FLAGS -Werror)
endif()

# --- Portable emulator core (header-only for now; becomes a static library
#     once the first .cpp files arrive in Milestone 2). -----------------------
add_library(g3core INTERFACE)
target_include_directories(g3core INTERFACE ${CMAKE_SOURCE_DIR})

# --- Portable host-side platform code (also compiled into the Circle kernel).
file(GLOB G3_PLATFORM_COMMON_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/platform/common/*.cpp)
add_library(g3platform_common STATIC ${G3_PLATFORM_COMMON_SOURCES})
target_link_libraries(g3platform_common PUBLIC g3core)
target_compile_options(g3platform_common PRIVATE ${G3_COMMON_FLAGS})

# --- Unit tests ---------------------------------------------------------------
file(GLOB_RECURSE G3_TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/tests/unit/*.cpp)

add_executable(g3_unit_tests
  tests/framework/g3test_main.cpp
  ${G3_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(g3_unit_tests PRIVATE g3core g3platform_common Threads::Threads)
target_compile_options(g3_unit_tests PRIVATE ${G3_COMMON_FLAGS})

if(G3_SANITIZE)
  target_compile_options(g3platform_common PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_compile_options(g3_unit_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(g3_unit_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME unit_tests COMMAND g3_unit_tests)
