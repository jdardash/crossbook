include(FetchContent)

# Catch2 v3. Pinned to an exact tag: a test framework that drifts under you
# turns a red build into an archaeology exercise.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

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
