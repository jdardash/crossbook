add_executable(crossbook_capture crossbook_capture.cpp)

target_link_libraries(crossbook_capture PRIVATE
    crossbook::crossbook
    crossbook::net
    crossbook_warnings
)
