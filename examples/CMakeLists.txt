# Examples exist so the README's code blocks are compiled rather than believed.
#
# Off for a consumer (the option defaults to CROSSBOOK_IS_TOP_LEVEL): somebody
# vendoring the library wants the headers, not our demo binaries in their build
# graph. On here, and registered with CTest when the suite is on, because an
# example that only compiles proves the signatures still exist and nothing else
# -- this one verifies real Kraken checksums and exits non-zero if it cannot.

add_executable(crossbook_example_verify_kraken_frames verify_kraken_frames.cpp)
target_link_libraries(crossbook_example_verify_kraken_frames PRIVATE
    crossbook::crossbook
    crossbook_warnings
)

if(CROSSBOOK_BUILD_TESTS)
    add_test(NAME example.verify_kraken_frames
             COMMAND crossbook_example_verify_kraken_frames)
endif()
