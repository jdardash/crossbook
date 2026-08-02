# Command-line tools. Not part of the library; they link it.
add_executable(crossbook_verify verify.cpp)
target_link_libraries(crossbook_verify PRIVATE crossbook::crossbook)
