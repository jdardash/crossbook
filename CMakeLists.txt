cmake_minimum_required(VERSION 3.24)

# The version this tree actually is, not the one it was when the file was
# written. The README describes L3, consolidated cross-venue books, capture and
# the websocket transport, none of which exist at the v0.1.0 tag its own
# FetchContent example pins — so a reader who followed the documented path got a
# checkout with no `crossbook/feed.hpp` in it. Three sources disagreed: this
# line, the newest tag (v0.2.0), and the README's Status heading (v0.3).
# 0.3.0 is the one that matches the feature set, and CROSSBOOK_CHECK_README_VERSION
# below exists so the three cannot drift apart again silently.
project(crossbook
    VERSION 0.3.0
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
option(CROSSBOOK_BUILD_TOOLS "Build the network tools (needs a TLS backend)"
       ${CROSSBOOK_IS_TOP_LEVEL})
option(CROSSBOOK_BUILD_BENCH "Build the benchmarks" OFF)
option(CROSSBOOK_BUILD_FUZZ "Build the fuzz targets (clang only)" OFF)
option(CROSSBOOK_BUILD_EXAMPLES "Build the compilable examples" ${CROSSBOOK_IS_TOP_LEVEL})
option(CROSSBOOK_WERROR "Treat warnings as errors" ${CROSSBOOK_IS_TOP_LEVEL})

# Off by default and never inherited: a binary tuned for the build machine's
# ISA is a binary that dies with an illegal instruction on the next machine,
# and the README's numbers must come from the configuration a consumer gets
# by default. This exists for measuring the ceiling on one's own hardware.
option(CROSSBOOK_NATIVE
       "Tune codegen for this machine (-march=native, /arch:AVX2 on MSVC) and enable LTO" OFF)

if(CROSSBOOK_NATIVE)
    if(MSVC)
        # MSVC has no -march=native; AVX2 is the widest ISA this project is
        # willing to assume behind an explicit opt-in.
        add_compile_options(/arch:AVX2)
    else()
        add_compile_options(-march=native)
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT crossbook_ipo_supported OUTPUT crossbook_ipo_message)
    if(crossbook_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "crossbook: LTO unavailable, continuing without it: ${crossbook_ipo_message}")
    endif()
endif()

# Installing is a decision for whoever owns the prefix. A parent project that
# vendors crossbook and then runs `cmake --install .` must not have crossbook's
# headers appear in its prefix as a side effect it never asked for, so the
# install rules exist only when crossbook is the project being built.
option(CROSSBOOK_INSTALL "Generate install rules and a find_package() config"
       ${CROSSBOOK_IS_TOP_LEVEL})

# Off by default because it reads a file the library does not need in order to
# build, and a consumer's configure must never fail over the state of our
# README. CI turns it on in a job of its own, which is what makes it a gate
# rather than a comment.
option(CROSSBOOK_CHECK_README_VERSION
       "Fail configure if the README's FetchContent GIT_TAG is not this version" OFF)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Marking a fetched dependency's headers as system, on the version floor
#
# `FetchContent_Declare(... SYSTEM)` is a CMake 3.25 feature and this project's
# documented floor is 3.24, so passing the keyword unconditionally makes the
# floor a fiction: the argument is forwarded to ExternalProject, which does not
# know it. The keyword is therefore used only where it exists, and on 3.24 the
# same effect is applied after the fact.
#
# This matters more now than it used to. Our own targets opt into
# -Wconversion/-Wshadow/-Werror explicitly, and Catch2's and Google Benchmark's
# headers are compiled into those targets; without the system marking their
# diagnostics become our build failures, and the wrong fix for that -- a
# target-wide -Wno- -- also silences the flag over our own code.
# ---------------------------------------------------------------------------
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    set(CROSSBOOK_FETCHCONTENT_SYSTEM SYSTEM)
else()
    set(CROSSBOOK_FETCHCONTENT_SYSTEM "")
endif()

function(crossbook_mark_system)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
        return()  # FetchContent_Declare(SYSTEM) already did it.
    endif()
    foreach(dependency IN LISTS ARGN)
        if(TARGET ${dependency})
            get_target_property(dirs ${dependency} INTERFACE_INCLUDE_DIRECTORIES)
            if(dirs)
                set_target_properties(${dependency} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
            endif()
        endif()
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Warning configuration
#
# These flags are a build-of-this-project concern and stop at this project's
# own targets. They used to hang off the INTERFACE library, which meant every
# consumer's own translation units inherited -Wconversion, -Wshadow,
# /permissive- and /utf-8 transitively: a desk with an existing codebase and
# -Werror got thousands of diagnostics in files that have nothing to do with
# order books, and /permissive- silently changed name lookup and two-phase
# template rules under them. An opinion about our own code is not something to
# export.
#
# Everything that compiles code *in this repository* opts in explicitly:
# tests/, bench/, fuzz/, src/net/ and tools/. That list is short enough to keep
# honest, and the alternative -- propagation -- is not something a consumer can
# opt out of.
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

# SYSTEM when somebody else is the project being built, plain when we are.
#
# A consumer who turns their own warnings up should see diagnostics about their
# code, not ours. Marking the include directory SYSTEM downgrades our headers to
# "not your problem" in their build, which is the same courtesy every dependency
# fetched here extends to us (Catch2 and Google Benchmark are both declared
# SYSTEM). At top level the marking is deliberately absent: our own tests and
# benchmarks must keep seeing every warning our headers produce, because that is
# the only build where anyone is going to fix them.
#
# find_package() consumers get this for nothing -- CMake treats an IMPORTED
# target's include directories as SYSTEM already -- so the flag only has to be
# spelled out for the FetchContent/add_subdirectory path.
if(CROSSBOOK_IS_TOP_LEVEL)
    set(CROSSBOOK_INCLUDE_SYSTEM "")
else()
    set(CROSSBOOK_INCLUDE_SYSTEM SYSTEM)
endif()

target_include_directories(crossbook ${CROSSBOOK_INCLUDE_SYSTEM} INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(crossbook INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# README / project version agreement
#
# The README tells a consumer to pin a tag. If that tag is not this version the
# instruction is wrong in the one place a reader has no way to check it -- they
# get a checkout, it configures, and the headers the next code block uses are
# simply absent. Comparing the two mechanically is four lines; the drift it
# prevents cost a reader an afternoon.
# ---------------------------------------------------------------------------
if(CROSSBOOK_CHECK_README_VERSION)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/README.md" _crossbook_readme)
    if(NOT _crossbook_readme MATCHES "GIT_TAG[ \t]+v([0-9]+\\.[0-9]+\\.[0-9]+)")
        message(FATAL_ERROR
            "crossbook: no `GIT_TAG v<x.y.z>` found in README.md. The FetchContent "
            "example is the documented consumption path; it cannot go missing.")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
        message(FATAL_ERROR
            "crossbook: README.md pins GIT_TAG v${CMAKE_MATCH_1} but project() says "
            "${PROJECT_VERSION}. One of them is lying to a reader who cannot tell "
            "which. Update README.md to v${PROJECT_VERSION}, or bump project(VERSION).")
    endif()
    message(STATUS "crossbook: README GIT_TAG matches project version ${PROJECT_VERSION}")
endif()

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

if(CROSSBOOK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Transport and tools
#
# Kept behind an option and in a separate target because this is the only part
# of the project with a platform dependency. Windows uses Schannel, which ships
# with the OS; everywhere else uses OpenSSL. Nothing here is reachable from the
# header-only library, so consuming `crossbook::crossbook` still costs nothing.
#
# When there is no TLS backend the tools are skipped with a message rather than
# built without TLS. Talking to an exchange in plaintext is not a degraded mode,
# it is a different and much worse program.
# ---------------------------------------------------------------------------
if(CROSSBOOK_BUILD_TOOLS)
    if(WIN32)
        set(CROSSBOOK_HAVE_TLS ON)
    else()
        find_package(OpenSSL)
        if(OpenSSL_FOUND)
            set(CROSSBOOK_HAVE_TLS ON)
        else()
            set(CROSSBOOK_HAVE_TLS OFF)
            message(STATUS
                "crossbook: OpenSSL not found - skipping the network tools. "
                "Install libssl-dev (Debian/Ubuntu) or openssl (Homebrew) to build them.")
        endif()
    endif()

    if(CROSSBOOK_HAVE_TLS)
        add_subdirectory(src/net)
        add_subdirectory(tools)
    endif()
endif()

if(CROSSBOOK_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install and package config
#
# `install(DIRECTORY include/)` alone copies headers and nothing else, which
# means `find_package(crossbook CONFIG REQUIRED)` -- the line every packaged
# dependency is consumed by -- finds nothing. vcpkg, Conan and a desk with its
# own /opt prefix all go through that call, so without the four files below the
# only remaining option is to vendor the headers by copy-paste, and a
# copy-pasted dependency is one that never gets updated again.
#
# ARCH_INDEPENDENT on the version file is not a detail. Without it CMake writes
# a `sizeof(void*)` guard into crossbookConfigVersion.cmake and rejects the
# package whenever the consumer's pointer size differs from whatever machine
# built the config -- for a header-only library that check is measuring nothing,
# and it is the classic way a header-only package becomes un-findable on a
# 32-bit or cross-compiled consumer.
#
# SameMinorVersion: 0.x is not stable, so 0.3 and 0.4 are treated as different
# libraries; within 0.3 a patch bump satisfies a 0.3.0 request.
# ---------------------------------------------------------------------------
if(CROSSBOOK_INSTALL)
    include(CMakePackageConfigHelpers)

    set(CROSSBOOK_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/crossbook")

    install(TARGETS crossbook EXPORT crossbookTargets)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT crossbookTargets
        FILE crossbookTargets.cmake
        NAMESPACE crossbook::
        DESTINATION ${CROSSBOOK_CMAKE_INSTALL_DIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/crossbookConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/crossbookConfig.cmake
        INSTALL_DESTINATION ${CROSSBOOK_CMAKE_INSTALL_DIR}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/crossbookConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
        ARCH_INDEPENDENT
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/crossbookConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/crossbookConfigVersion.cmake
        DESTINATION ${CROSSBOOK_CMAKE_INSTALL_DIR}
    )
endif()
