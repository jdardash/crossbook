include(FetchContent)

# Catch2 v3. Pinned to an exact tag: a test framework that drifts under you
# turns a red build into an archaeology exercise.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
    GIT_SHALLOW TRUE
    ${CROSSBOOK_FETCHCONTENT_SYSTEM}
)
FetchContent_MakeAvailable(Catch2)
crossbook_mark_system(Catch2 Catch2WithMain)

add_executable(crossbook_tests
    test_fixed.cpp
    test_checksum.cpp
    test_book.cpp
    test_book_equivalence.cpp
    test_sequence.cpp
    test_no_alloc.cpp
    test_determinism.cpp
    test_histogram.cpp
    test_json.cpp
    test_venues.cpp
    test_feed.cpp
    test_replay.cpp
    test_execution.cpp
    test_consolidated.cpp
    test_l3.cpp
    test_ws_frame.cpp
    test_url.cpp
    test_handshake.cpp
    test_capture.cpp
    test_depth_trim.cpp
    test_fixture_replay.cpp
)

target_link_libraries(crossbook_tests PRIVATE
    crossbook::crossbook
    crossbook_warnings
    Catch2::Catch2WithMain
)

# Somewhere writable for the capture round-trip test. The build tree, so a test
# run leaves nothing behind in the source tree or in a shared temp directory.
target_compile_definitions(crossbook_tests PRIVATE
    CROSSBOOK_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    # The committed capture the end-to-end replay test runs against. Baked in as
    # an absolute path so the test does not depend on the working directory it
    # happens to be launched from.
    CROSSBOOK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(crossbook_tests)

# ---------------------------------------------------------------------------
# Self-containment
#
# The single highest-leverage test a header-only library has, and the one this
# suite structurally could not perform: every test TU includes a Catch2 header
# first, and Catch2 transitively drags in most of the standard library, so a
# header that forgot <cstddef> or <utility> compiled here and failed in a
# consumer's file that happened to include it first. Three headers were never
# included directly by any test at all, so nothing compiled them in isolation
# even by accident.
#
# One generated TU per header, that header first and nothing before it. Included
# twice, which also proves `#pragma once` is doing its job -- a header that is
# not idempotent breaks the first consumer who includes it from two places.
#
# GLOB_RECURSE rather than a hand-maintained list on purpose: a list you have to
# remember to extend is exactly how a new header arrives untested. CONFIGURE_DEPENDS
# makes CMake re-run when the set of headers changes.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CROSSBOOK_PUBLIC_HEADERS
    RELATIVE "${PROJECT_SOURCE_DIR}/include"
    CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/crossbook/*.hpp")

set(CROSSBOOK_SELFCONTAINED_DIR "${CMAKE_CURRENT_BINARY_DIR}/selfcontained")
set(CROSSBOOK_SELFCONTAINED_SRCS "")
foreach(header IN LISTS CROSSBOOK_PUBLIC_HEADERS)
    string(REGEX REPLACE "[/.]" "_" stem "${header}")
    set(generated "${CROSSBOOK_SELFCONTAINED_DIR}/${stem}.cpp")
    file(WRITE "${generated}" "#include \"${header}\"\n#include \"${header}\"\n")
    list(APPEND CROSSBOOK_SELFCONTAINED_SRCS "${generated}")
endforeach()

add_library(crossbook_selfcontained OBJECT ${CROSSBOOK_SELFCONTAINED_SRCS})
target_link_libraries(crossbook_selfcontained PRIVATE crossbook::crossbook crossbook_warnings)

# The <windows.h> hazard, pinned.
#
# windows.h defines object-like `min` and `max` macros unless NOMINMAX is set,
# and a consumer who has already included it -- which on a Windows desk is most
# of them -- turns every unparenthesised `std::min(a, b)` and
# `numeric_limits<T>::max()` in our headers into an unreadable error *inside our
# files*, which reads to them as "crossbook is broken". The fix is
# parenthesisation, `(std::min)(a, b)`, and the fix is invisible: nothing in a
# normal build notices when someone writes the plain form again. This TU
# notices, on the only platform where it matters, and it deliberately does not
# define NOMINMAX because defining it is exactly the workaround we must not
# require of a consumer.
if(WIN32)
    set(CROSSBOOK_WINDOWS_TU "${CROSSBOOK_SELFCONTAINED_DIR}/windows_h_no_nominmax.cpp")
    set(CROSSBOOK_WINDOWS_TU_BODY "#include <windows.h>\n")
    foreach(header IN LISTS CROSSBOOK_PUBLIC_HEADERS)
        string(APPEND CROSSBOOK_WINDOWS_TU_BODY "#include \"${header}\"\n")
    endforeach()
    file(WRITE "${CROSSBOOK_WINDOWS_TU}" "${CROSSBOOK_WINDOWS_TU_BODY}")
    target_sources(crossbook_selfcontained PRIVATE "${CROSSBOOK_WINDOWS_TU}")
endif()
