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
)

target_link_libraries(crossbook_tests PRIVATE
    crossbook::crossbook
    Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(crossbook_tests)
