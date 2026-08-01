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
    SYSTEM
)
FetchContent_MakeAvailable(benchmark)

add_executable(crossbook_bench bench_book.cpp)
target_link_libraries(crossbook_bench PRIVATE
    crossbook::crossbook
    benchmark::benchmark
)

# Google Benchmark's headers trip our stricter warning set; the library's own
# code is not ours to fix, and -Werror on it would just block the build.
if(NOT MSVC)
    target_compile_options(crossbook_bench PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()
