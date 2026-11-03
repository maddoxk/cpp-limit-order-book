cmake_minimum_required(VERSION 3.20)
project(cpp_limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests (fetches GoogleTest)" ON)
option(LOB_BUILD_BENCH "Build benchmarks (fetches Google Benchmark)" OFF)
option(LOB_BUILD_TOOLS "Build the feed generator and replay tools" ON)
# e.g. -DLOB_SANITIZE=address,undefined  or  -DLOB_SANITIZE=thread
set(LOB_SANITIZE "" CACHE STRING "Comma separated list passed to -fsanitize=")

# Sanitizer flags are applied globally (before FetchContent) so GoogleTest and
# Google Benchmark are instrumented the same way as our code.
if(LOB_SANITIZE)
  add_compile_options(-fsanitize=${LOB_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LOB_SANITIZE})
  if(LOB_SANITIZE MATCHES "undefined")
    # Make UB findings fail the test instead of just printing.
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()

# Warning flags are attached to our own targets only (not third-party code).
add_library(lob_options INTERFACE)
target_compile_options(lob_options INTERFACE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(lob
  src/order_book.cpp
  src/messages.cpp
  src/generator.cpp
  src/replay.cpp)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC lob_options Threads::Threads)

if(LOB_BUILD_TOOLS)
  add_executable(lob_gen tools/lob_gen.cpp)
  target_link_libraries(lob_gen PRIVATE lob)
  add_executable(lob_replay tools/lob_replay.cpp)
  target_link_libraries(lob_replay PRIVATE lob)
endif()

include(FetchContent)

if(LOB_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()
