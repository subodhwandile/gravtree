cmake_minimum_required(VERSION 3.20)
project(gravtree VERSION 0.1.0 LANGUAGES CXX)

# ── C++ standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compile flags ────────────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native"          CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined"   CACHE STRING "" FORCE)

# ── Core library ─────────────────────────────────────────────────────────────
add_library(gravtree_lib
    src/bloom.cpp
    src/node.cpp
    src/manifest.cpp
    src/flush.cpp
    src/skiplist.cpp
    src/tree.cpp
)

target_include_directories(gravtree_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(gravtree_lib PUBLIC pthread)

# ── Dependencies (fetched at configure time) ─────────────────────────────────
include(FetchContent)

# GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Google Benchmark
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
    GIT_SHALLOW    TRUE
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest googlebenchmark)

# ── Sub-targets ───────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS gravtree_lib EXPORT gravtreeTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
