cmake_minimum_required(VERSION 3.20)
project(custom-memory-manager CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# FetchContent dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0)

FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3)

FetchContent_Declare(rapidcheck
  GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
  GIT_TAG        master)

# Prevent GoogleTest from overriding our compiler/linker options
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Disable benchmark tests and docs to speed up the build
set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS    OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest benchmark rapidcheck)

# ---------------------------------------------------------------------------
# memory — static library
# ---------------------------------------------------------------------------
file(GLOB MEMORY_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(memory STATIC ${MEMORY_SOURCES})

target_include_directories(memory PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(memory PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# memory_tests — test executable
# ---------------------------------------------------------------------------
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)

add_executable(memory_tests ${TEST_SOURCES})

target_include_directories(memory_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(memory_tests PRIVATE
  GTest::gtest_main
  rapidcheck
  memory
)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(memory_tests)

# ---------------------------------------------------------------------------
# memory_benchmarks — benchmark executable
# ---------------------------------------------------------------------------
file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS benchmarks/*.cpp)

add_executable(memory_benchmarks ${BENCH_SOURCES})

target_include_directories(memory_benchmarks PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(memory_benchmarks PRIVATE
  benchmark::benchmark_main
  memory
)
