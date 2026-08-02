# The transport library.
#
# Separate from `crossbook` on purpose: the library proper is header-only and
# depends on nothing, and consuming it must not drag in a TLS stack. Only the
# tools link this.

add_library(crossbook_net STATIC
    tcp_socket.cpp
    transport.cpp
    websocket.cpp
)
add_library(crossbook::net ALIAS crossbook_net)

target_link_libraries(crossbook_net
    PUBLIC crossbook::crossbook
    PRIVATE crossbook_warnings
)

# The internal headers (tcp_socket.hpp, tls_backend.hpp) sit next to the sources
# and are deliberately not installed.
target_include_directories(crossbook_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_sources(crossbook_net PRIVATE tls_schannel.cpp)
    # secur32 is Schannel/SSPI; crypt32 backs certificate validation; ws2_32 is
    # the socket layer. All three ship with the OS.
    target_link_libraries(crossbook_net PRIVATE ws2_32 secur32 crypt32)
else()
    target_sources(crossbook_net PRIVATE tls_openssl.cpp)
    target_link_libraries(crossbook_net PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
