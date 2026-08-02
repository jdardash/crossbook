// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The plaintext transport, the factory, and the one-shot HTTPS GET used to
// fetch Binance's REST depth snapshot.

#include "crossbook/net/transport.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "tcp_socket.hpp"
#include "tls_backend.hpp"

namespace crossbook::net {
namespace {

/// TCP with no TLS. Kept because a local test server is plaintext and because
/// having the plain path exercised keeps the TLS backends honest about what
/// they are actually adding.
class PlainTransport final : public Transport {
public:
    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port,
                               int timeout_ms) override {
        if (!socket_.connect(host, port, timeout_ms, error_)) {
            return false;
        }
        connected_ = true;
        return true;
    }

    [[nodiscard]] IoStatus read(char* buf, std::size_t len, std::size_t& n_read) override {
        const IoStatus status = socket_.read(buf, len, n_read, error_);
        if (status == IoStatus::kClosed || status == IoStatus::kError) {
            connected_ = false;
        }
        return status;
    }

    [[nodiscard]] IoStatus write(const char* buf, std::size_t len) override {
        const IoStatus status = socket_.write(buf, len, error_);
        if (status == IoStatus::kClosed || status == IoStatus::kError) {
            connected_ = false;
        }
        return status;
    }

    void set_read_timeout(int timeout_ms) override { socket_.set_read_timeout(timeout_ms); }

    void close() override {
        socket_.close();
        connected_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }
    [[nodiscard]] const std::string& last_error() const noexcept override { return error_; }

private:
    detail::TcpSocket socket_;
    std::string error_;
    bool connected_{false};
};

/// Case-insensitive header lookup over a raw HTTP header block.
[[nodiscard]] std::string_view http_header(std::string_view headers, std::string_view name) {
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::string_view line =
            headers.substr(pos, (eol == std::string_view::npos ? headers.size() : eol) - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && colon == name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (lower(line[i]) != lower(name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                return value;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = eol + 2;
    }
    return {};
}

/// Decode HTTP/1.1 chunked transfer coding.
///
/// Binance answers depth requests with Content-Length in practice, but a proxy
/// or a CDN in front of it may re-chunk the response, and a body silently
/// truncated at the first chunk header decodes into a snapshot that looks
/// plausible and is missing most of the book.
[[nodiscard]] bool decode_chunked(std::string_view input, std::string& out) {
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t eol = input.find("\r\n", pos);
        if (eol == std::string_view::npos) {
            return false;
        }
        // The size line may carry chunk extensions after a ';'.
        std::string_view size_text = input.substr(pos, eol - pos);
        const std::size_t semi = size_text.find(';');
        if (semi != std::string_view::npos) {
            size_text = size_text.substr(0, semi);
        }

        std::size_t chunk_size = 0;
        if (size_text.empty()) {
            return false;
        }
        for (const char c : size_text) {
            std::size_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::size_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::size_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::size_t>(c - 'A' + 10);
            } else {
                return false;
            }
            chunk_size = chunk_size * 16 + digit;
        }

        pos = eol + 2;
        if (chunk_size == 0) {
            return true;  // Terminal chunk; trailers ignored.
        }
        if (pos + chunk_size > input.size()) {
            return false;
        }
        out.append(input.substr(pos, chunk_size));
        pos += chunk_size + 2;  // Skip the chunk's trailing CRLF.
    }
    return false;
}

}  // namespace

std::unique_ptr<Transport> make_transport(bool secure) {
    if (!secure) {
        return std::make_unique<PlainTransport>();
    }
    return detail::make_tls_transport();
}

bool https_get(const std::string& host, const std::string& path, std::string& body,
               std::string& error, int timeout_ms) {
    body.clear();
    error.clear();

    auto transport = make_transport(true);
    if (!transport) {
        error = "no TLS backend in this build";
        return false;
    }
    if (!transport->connect(host, 443, timeout_ms)) {
        error = transport->last_error();
        return false;
    }

    std::string request;
    request.append("GET ").append(path).append(" HTTP/1.1\r\n");
    request.append("Host: ").append(host).append("\r\n");
    request.append("User-Agent: crossbook/0.2\r\n");
    request.append("Accept: application/json\r\n");
    // No keep-alive: one request per connection means the end of the body is
    // unambiguous even if the server omits Content-Length.
    request.append("Connection: close\r\n\r\n");

    if (transport->write(request.data(), request.size()) != IoStatus::kOk) {
        error = transport->last_error().empty() ? "http write failed" : transport->last_error();
        return false;
    }

    // A depth snapshot is a few hundred kilobytes; the ceiling is here so a
    // misbehaving endpoint cannot make this loop until memory runs out.
    constexpr std::size_t kMaxResponse = 64U * 1024U * 1024U;

    std::string response;
    char chunk[16384];
    for (;;) {
        std::size_t got = 0;
        const IoStatus status = transport->read(chunk, sizeof(chunk), got);
        if (status == IoStatus::kOk) {
            response.append(chunk, got);
            if (response.size() > kMaxResponse) {
                error = "http response exceeded ceiling";
                return false;
            }
            continue;
        }
        if (status == IoStatus::kClosed) {
            break;
        }
        if (status == IoStatus::kTimeout) {
            error = "http read timed out";
            return false;
        }
        error = transport->last_error().empty() ? "http read failed" : transport->last_error();
        return false;
    }
    transport->close();

    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "http response had no header terminator";
        return false;
    }

    const std::string_view head(response.data(), header_end);
    const std::size_t status_sp = head.find(' ');
    if (status_sp == std::string_view::npos) {
        error = "http response had no status line";
        return false;
    }
    const std::string_view code = head.substr(status_sp + 1, 3);
    if (code != "200") {
        error = "http status " + std::string(code);
        return false;
    }

    const std::string_view raw_body(response.data() + header_end + 4,
                                    response.size() - header_end - 4);

    if (http_header(head, "transfer-encoding").find("chunked") != std::string_view::npos) {
        if (!decode_chunked(raw_body, body)) {
            error = "malformed chunked body";
            return false;
        }
        return true;
    }

    body.assign(raw_body);
    return true;
}

}  // namespace crossbook::net
