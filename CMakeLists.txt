cmake_minimum_required(VERSION 3.25)  # first CMake that knows CMAKE_CXX_STANDARD 26
project(spsc_ring_buffer VERSION 2.0.0 LANGUAGES CXX)

# C++26: contracts, static reflection, expansion statements, = delete("why"),
# std::inplace_vector, saturation arithmetic. Needs GCC 16 or newer.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SPSC_BUILD_BENCHMARKS "Build the benchmark and profiling binaries" ON)
option(SPSC_BUILD_TESTS "Build the unit tests" ON)
option(SPSC_NATIVE "Tune for the build machine (-march=native)" ON)
set(SPSC_SANITIZE "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or empty")

# ---- the library: header-only ------------------------------------------------
add_library(spsc INTERFACE)
add_library(spsc::spsc ALIAS spsc)
target_include_directories(spsc INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(spsc INTERFACE cxx_std_26)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "spsc needs GCC 16+ for C++26 contracts and reflection "
                      "(found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# ---- common flags for everything we compile ---------------------------------
add_library(spsc_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(spsc_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
  # C++26 static reflection (harness/describe.hpp) is behind a flag in GCC 16.
  target_compile_options(spsc_flags INTERFACE -freflection)
  # GCC 16 keeps the contract-violation handler and <debugging> in a separate
  # library. Without it the link fails, and on MinGW collect2 hides the
  # "undefined reference" message.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(spsc_flags INTERFACE stdc++exp)
  endif()
  # Frame pointers and debug info cost ~nothing here and make profiles readable.
  target_compile_options(spsc_flags INTERFACE -g -fno-omit-frame-pointer)
  if(SPSC_NATIVE)
    target_compile_options(spsc_flags INTERFACE -march=native)
  endif()
  if(SPSC_SANITIZE)
    target_compile_options(spsc_flags INTERFACE -fsanitize=${SPSC_SANITIZE})
    target_link_options(spsc_flags INTERFACE -fsanitize=${SPSC_SANITIZE})
  endif()
  if(MINGW)
    # Self-contained executables: no dependency on the MSYS2 runtime DLLs.
    target_link_options(spsc_flags INTERFACE -static)
  endif()
elseif(MSVC)
  target_compile_options(spsc_flags INTERFACE /W4 /permissive-)
endif()

if(NOT SPSC_BUILD_BENCHMARKS AND NOT SPSC_BUILD_TESTS)
  return()
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---- benchmark harness --------------------------------------------------------
if(SPSC_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(benchmark)

  string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
  set(_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}")
  if(SPSC_NATIVE)
    string(APPEND _flags " -march=native")
  endif()
  string(APPEND _flags " -g -fno-omit-frame-pointer -std=c++26 -freflection")
  string(APPEND _flags " -fcontract-evaluation-semantic=ignore")
  string(STRIP "${_flags}" _flags)

  add_library(spsc_harness STATIC
    harness/platform.cpp
    harness/latency.cpp
    harness/sampler.cpp)
  target_include_directories(spsc_harness PUBLIC harness)
  target_link_libraries(spsc_harness PUBLIC spsc spsc_flags)
  target_compile_definitions(spsc_harness PRIVATE
    SPSC_BUILD_FLAGS="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${_flags}")
  if(WIN32)
    target_link_libraries(spsc_harness PUBLIC dbghelp)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(spsc_harness PUBLIC Threads::Threads)

  add_executable(spsc_bench bench/bench_main.cpp)
  target_link_libraries(spsc_bench PRIVATE spsc_harness benchmark::benchmark)

  add_executable(spsc_profile bench/profile_main.cpp)
  target_link_libraries(spsc_profile PRIVATE spsc_harness)

  # Contracts are compiled out of everything that is measured: a precondition
  # check on pop() would add a cross-core load to the hot path.
  foreach(t spsc_harness spsc_bench spsc_profile)
    target_compile_options(${t} PRIVATE -fcontract-evaluation-semantic=ignore)
  endforeach()
endif()

# ---- tests --------------------------------------------------------------------
if(SPSC_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(spsc_tests tests/test_ring_buffer.cpp)
  target_link_libraries(spsc_tests PRIVATE spsc spsc_flags GTest::gtest_main Threads::Threads)
  # Contracts are checked in tests with the standard "enforce" semantic:
  # a violation prints the failed predicate and terminates.
  target_compile_options(spsc_tests PRIVATE -fcontract-evaluation-semantic=enforce)
  include(GoogleTest)
  gtest_discover_tests(spsc_tests DISCOVERY_TIMEOUT 60)
endif()
