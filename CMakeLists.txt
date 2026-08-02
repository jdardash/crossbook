cmake_minimum_required(VERSION 3.24)

project(crossbook
    VERSION 0.1.0
    DESCRIPTION "Multi-venue crypto order book reconstruction with continuous exchange-checksum verification"
    HOMEPAGE_URL "https://github.com/jdardash/crossbook"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd, clang-tidy, and IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Treat this as the top-level project only when it actually is; when vendored
# via FetchContent or add_subdirectory, tests and benchmarks stay off.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CROSSBOOK_IS_TOP_LEVEL ON)
else()
    set(CROSSBOOK_IS_TOP_LEVEL OFF)
endif()

option(CROSSBOOK_BUILD_TESTS "Build the test suite" ${CROSSBOOK_IS_TOP_LEVEL})
option(CROSSBOOK_BUILD_BENCH "Build the benchmarks" OFF)
option(CROSSBOOK_BUILD_FUZZ "Build the fuzz targets (clang only)" OFF)
option(CROSSBOOK_BUILD_TOOLS "Build the command-line tools" ${CROSSBOOK_IS_TOP_LEVEL})
option(CROSSBOOK_WERROR "Treat warnings as errors" ${CROSSBOOK_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Warning configuration
#
# The library is header-only today, so warnings are attached to an INTERFACE
# target and inherited by anything that links it. -Werror is on by default for
# top-level builds: a warning that is allowed to persist is a warning nobody
# reads.
# ---------------------------------------------------------------------------
add_library(crossbook_warnings INTERFACE)

if(MSVC)
    target_compile_options(crossbook_warnings INTERFACE
        /W4
        /permissive-        # Standards conformance; rejects MSVC-only laxity.
        /Zc:__cplusplus     # Report the real __cplusplus value.
        /Zc:preprocessor    # Conforming preprocessor.
        /utf-8
    )
    if(CROSSBOOK_WERROR)
        target_compile_options(crossbook_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(crossbook_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion          # Silent narrowing is how price data gets corrupted.
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wdouble-promotion
        -Wformat=2
    )
    if(CROSSBOOK_WERROR)
        target_compile_options(crossbook_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(crossbook INTERFACE)
add_library(crossbook::crossbook ALIAS crossbook)

target_include_directories(crossbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(crossbook INTERFACE cxx_std_20)
target_link_libraries(crossbook INTERFACE crossbook_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CROSSBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CROSSBOOK_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(CROSSBOOK_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

if(CROSSBOOK_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
