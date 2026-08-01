foreach(tool crossbook_capture crossbook_verify)
    add_executable(${tool} ${tool}.cpp)
    target_link_libraries(${tool} PRIVATE
        crossbook::crossbook
        crossbook::net
        crossbook_warnings
    )
endforeach()
