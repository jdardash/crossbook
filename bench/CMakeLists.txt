include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
    GIT_SHALLOW TRUE
    ${CROSSBOOK_FETCHCONTENT_SYSTEM}
)
FetchContent_MakeAvailable(benchmark)
crossbook_mark_system(benchmark benchmark_main)

add_executable(crossbook_bench bench_book.cpp)

# The warning set is opted into here rather than inherited from the library,
# because the library no longer exports it. bench_book.cpp is our code and gets
# held to our rules.
#
# The -Wno-conversion/-Wno-sign-conversion pair that used to sit here is gone.
# It was added to quieten Google Benchmark's headers, but a target-wide -Wno-
# silences the flag over *our* translation unit too, and bench_book.cpp is
# nothing but integer arithmetic on price mantissas -- precisely the code
# -Wconversion exists to police. Google Benchmark is declared SYSTEM above,
# which is the tool that suppresses a dependency's diagnostics without
# suppressing ours.
target_link_libraries(crossbook_bench PRIVATE
    crossbook::crossbook
    crossbook_warnings
    benchmark::benchmark
)
