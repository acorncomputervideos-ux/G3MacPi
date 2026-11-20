# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host (Linux) build: compiles the portable emulator core and runs unit tests.
# The bare-metal Raspberry Pi build uses Circle's make system instead
# (see app/circle/Makefile and tools/build-pi.sh).

cmake_minimum_required(VERSION 3.16)
project(G3MacPi LANGUAGES C CXX)

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

# --- Portable emulator core ----------------------------------------------------
file(GLOB_RECURSE G3_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/*.cpp)
add_library(g3core STATIC ${G3_CORE_SOURCES})
target_include_directories(g3core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(g3core PRIVATE ${G3_COMMON_FLAGS})

# --- Portable host-side platform code (also compiled into the Circle kernel).
file(GLOB G3_PLATFORM_COMMON_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/platform/common/*.cpp)
add_library(g3platform_common STATIC ${G3_PLATFORM_COMMON_SOURCES})
target_link_libraries(g3platform_common PUBLIC g3core)
target_compile_options(g3platform_common PRIVATE ${G3_COMMON_FLAGS})

# --- Host-side build tools for guest code (PEF writer) ---------------------------
add_library(g3pef STATIC tools/pef/pef_writer.cpp tools/pef/elf2pef.cpp)
# Simulator for the JIT's AArch64 code on other hosts (docs/cpu/jit-a64.md).
add_library(g3a64sim STATIC tools/a64sim/a64_sim.cpp)
target_include_directories(g3a64sim PUBLIC ${CMAKE_SOURCE_DIR})
add_executable(g3_rom_probe tools/probe/rom_probe.cpp)
target_link_libraries(g3_rom_probe PRIVATE g3core g3a64sim)
target_compile_options(g3_rom_probe PRIVATE ${G3_COMMON_FLAGS})
add_executable(g3_hfs tools/hfs/hfs_tool.cpp)
target_link_libraries(g3_hfs PRIVATE g3core)
target_compile_options(g3_hfs PRIVATE ${G3_COMMON_FLAGS})
add_executable(g3_host app/host/main.cpp)
target_link_libraries(g3_host PRIVATE g3platform_common g3a64sim)
target_compile_options(g3_host PRIVATE ${G3_COMMON_FLAGS})
add_executable(g3_mkdisk tools/diskimage/mkdisk.cpp)
target_link_libraries(g3_mkdisk PRIVATE g3core)
target_compile_options(g3_mkdisk PRIVATE ${G3_COMMON_FLAGS})
add_executable(g3_elf2pef tools/pef/elf2pef_main.cpp)
target_link_libraries(g3_elf2pef PRIVATE g3pef)
target_include_directories(g3pef PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(g3pef PRIVATE ${G3_COMMON_FLAGS})

# --- Unit tests ---------------------------------------------------------------
file(GLOB_RECURSE G3_TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/tests/unit/*.cpp)

add_executable(g3_unit_tests
  tests/framework/g3test_main.cpp
  ${G3_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(g3_unit_tests PRIVATE g3core g3platform_common g3pef g3a64sim Threads::Threads)
target_compile_definitions(g3_unit_tests PRIVATE G3_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# The FPU oracle test changes the host rounding mode at run time.
set_source_files_properties(tests/unit/fpu/test_ieee754_oracle.cpp PROPERTIES
  COMPILE_OPTIONS "-frounding-math")
target_compile_options(g3_unit_tests PRIVATE ${G3_COMMON_FLAGS})

if(G3_SANITIZE)
  target_compile_options(g3core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  # Anything linking the instrumented core needs the sanitizer runtime.
  target_link_options(g3core PUBLIC -fsanitize=address,undefined)
  target_compile_options(g3platform_common PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_compile_options(g3pef PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(g3pef PUBLIC -fsanitize=address,undefined)
  target_compile_options(g3_unit_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(g3_unit_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME unit_tests COMMAND g3_unit_tests)

# --- Benchmarks (not run by CI's test step) ------------------------------------
add_executable(g3_cpu_bench tools/bench/cpu_bench.cpp)
target_link_libraries(g3_cpu_bench PRIVATE g3core)
target_compile_options(g3_cpu_bench PRIVATE ${G3_COMMON_FLAGS})

# --- Guest-code tests (only with the PowerPC cross-compiler) ---------------------
# guest/tests/kernels.c is compiled for the emulated 750 and for the host;
# g3_guest_tests runs the guest build on the emulator and compares results.
find_program(G3_PPC_GCC powerpc-linux-gnu-gcc)
if(G3_PPC_GCC)
  set(G3_GUEST_FLAGS -m32 -mbig-endian -mcpu=750 -ffreestanding -fno-builtin -nostdlib -static
      -O2 -msdata=none -ffp-contract=off -Wall -Wextra -Werror -I${CMAKE_SOURCE_DIR})
  set(G3_KERNELS_ELF ${CMAKE_BINARY_DIR}/guest/kernels.elf)
  add_custom_command(
    OUTPUT ${G3_KERNELS_ELF}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/guest
    COMMAND ${G3_PPC_GCC} ${G3_GUEST_FLAGS} -Wl,-N -Wl,-Ttext=0x10000 -Wl,--build-id=none
            ${CMAKE_SOURCE_DIR}/guest/tests/start.S ${CMAKE_SOURCE_DIR}/guest/tests/kernels.c
            -lgcc -o ${G3_KERNELS_ELF}
    DEPENDS guest/tests/start.S guest/tests/kernels.c guest/tests/kernels.h
    COMMENT "Building guest test kernels for the PowerPC 750")
  add_custom_target(g3_guest_kernels DEPENDS ${G3_KERNELS_ELF})
  add_executable(g3_guest_tests tests/guest/run_kernels.cpp guest/tests/kernels.c)
  set_source_files_properties(guest/tests/kernels.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
  target_link_libraries(g3_guest_tests PRIVATE g3core)
  target_compile_options(g3_guest_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${G3_COMMON_FLAGS}>)
  add_dependencies(g3_guest_tests g3_guest_kernels)
  if(G3_SANITIZE)
    target_compile_options(g3_guest_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(g3_guest_tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME guest_kernels COMMAND g3_guest_tests ${G3_KERNELS_ELF})

  # guest/cfm/demo.c as a Mac OS code fragment: -fPIC -shared, converted by
  # g3_elf2pef; g3_guest_cfm prepares it like the Code Fragment Manager and
  # calls it on the emulated 750 (docs/tools/pef.md).
  set(G3_FRAGMENT_FLAGS -m32 -mbig-endian -mcpu=750 -ffreestanding -fno-builtin -nostdlib -O2
      -msdata=none -fPIC -shared -Wall -Wextra -Werror
      -Wl,-Bsymbolic -Wl,-z,max-page-size=16 -Wl,-z,common-page-size=16 -Wl,-z,norelro
      -Wl,--hash-style=sysv -Wl,--build-id=none)
  set(G3_DEMO_SO ${CMAKE_BINARY_DIR}/guest/cfm_demo.so)
  set(G3_DEMO_PEF ${CMAKE_BINARY_DIR}/guest/cfm_demo.pef)
  add_custom_command(
    OUTPUT ${G3_DEMO_PEF}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/guest
    COMMAND ${G3_PPC_GCC} ${G3_FRAGMENT_FLAGS} ${CMAKE_SOURCE_DIR}/guest/cfm/demo.c
            ${CMAKE_SOURCE_DIR}/guest/cfm/cfm_glue.S -lgcc -o ${G3_DEMO_SO}
    COMMAND g3_elf2pef ${G3_DEMO_SO} ${G3_DEMO_PEF} --export demo_entry --export demo_calls
            --export table:data --import imp_Callee=TestLib:Callee
    DEPENDS guest/cfm/demo.c guest/cfm/cfm_glue.S guest/cfm/cfm.h g3_elf2pef
    COMMENT "Building the CFM demo fragment")
  add_custom_target(g3_guest_cfm_demo DEPENDS ${G3_DEMO_PEF})
  add_executable(g3_guest_cfm tests/guest/run_cfm.cpp)
  target_link_libraries(g3_guest_cfm PRIVATE g3core g3pef)
  target_compile_options(g3_guest_cfm PRIVATE ${G3_COMMON_FLAGS})
  add_dependencies(g3_guest_cfm g3_guest_cfm_demo)
  if(G3_SANITIZE)
    target_compile_options(g3_guest_cfm PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(g3_guest_cfm PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME guest_cfm COMMAND g3_guest_cfm ${G3_DEMO_PEF} ${G3_DEMO_SO})

  # Our Mac OS display driver (guest/ndrv, docs/guest/ndrv.md), and its
  # test: g3_guest_ndrv plays Mac OS and drives it on the emulated 750.
  set(G3_NDRV_SO ${CMAKE_BINARY_DIR}/guest/g3_ndrv.so)
  set(G3_NDRV_PEF ${CMAKE_BINARY_DIR}/guest/g3_ndrv.pef)
  add_custom_command(
    OUTPUT ${G3_NDRV_PEF}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/guest
    COMMAND ${G3_PPC_GCC} ${G3_FRAGMENT_FLAGS} -I${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/guest/ndrv/g3_ndrv.c
            ${CMAKE_SOURCE_DIR}/guest/cfm/cfm_glue.S -lgcc -o ${G3_NDRV_SO}
    COMMAND g3_elf2pef ${G3_NDRV_SO} ${G3_NDRV_PEF} --export DoDriverIO --export TheDriverDescription:data
            --import imp_IOCommandIsComplete=DriverServicesLib:IOCommandIsComplete
            --import imp_RegistryPropertyGetSize=NameRegistryLib:RegistryPropertyGetSize
            --import imp_RegistryPropertyGet=NameRegistryLib:RegistryPropertyGet
    DEPENDS guest/ndrv/g3_ndrv.c guest/cfm/cfm_glue.S guest/cfm/cfm.h g3_elf2pef
    COMMENT "Building the Mac OS display driver")
  add_custom_target(g3_ndrv DEPENDS ${G3_NDRV_PEF})
  add_executable(g3_guest_ndrv tests/guest/run_ndrv.cpp)
  target_link_libraries(g3_guest_ndrv PRIVATE g3core g3pef)
  target_compile_options(g3_guest_ndrv PRIVATE ${G3_COMMON_FLAGS})
  add_dependencies(g3_guest_ndrv g3_ndrv)
  if(G3_SANITIZE)
    target_compile_options(g3_guest_ndrv PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(g3_guest_ndrv PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME guest_ndrv COMMAND g3_guest_ndrv ${G3_NDRV_PEF})
endif()
