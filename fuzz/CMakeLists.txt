# libFuzzer targets. Clang only — GCC and MSVC do not ship -fsanitize=fuzzer.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "CROSSBOOK_BUILD_FUZZ requires Clang; skipping fuzz targets.")
    return()
endif()

function(add_fuzz_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE crossbook::crossbook)
    # Fuzzing without a sanitizer finds crashes but not the memory errors that
    # precede them, which is most of the value.
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

add_fuzz_target(fuzz_parse_fixed fuzz_parse_fixed.cpp)
add_fuzz_target(fuzz_book fuzz_book.cpp)
add_fuzz_target(fuzz_sequence fuzz_sequence.cpp)
add_fuzz_target(fuzz_decode fuzz_decode.cpp)
add_fuzz_target(fuzz_l3 fuzz_l3.cpp)
add_fuzz_target(fuzz_ws_frame fuzz_ws_frame.cpp)
