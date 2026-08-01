// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include "crossbook/net/websocket.hpp"

#include <array>
#include <cstring>
#include <string>

#include "crossbook/net/handshake.hpp"

namespace crossbook::net {
namespace {

/// Read buffer for the transport. Sized so a burst of small messages is picked
/// up in one syscall without the buffer itself dominating the working set.
constexpr std::size_t kReadChunk = 32 * 1024;

/// Seed a PRNG from the platform entropy source.
///
/// The masking key must be unpredictable to the server (§5.3), which rules out
/// a fixed seed — including the "deterministic for tests" fixed seed that would
/// otherwise be this repository's habit. Determinism is enforced at the book
/// layer, where it is a correctness property; here it would be a defect.
[[nodiscard]] std::mt19937 seeded_rng() {
    std::random_device device;
    std::array<std::uint32_t, 8> seed_data{};
    for (auto& value : seed_data) {
        value = device();
    }
    std::seed_seq seq(seed_data.begin(), seed_data.end());
    return std::mt19937(seq);
}

}  // namespace

WebSocketClient::WebSocketClient(std::size_t max_message_bytes)
    : reader_(max_message_bytes), rng_(seeded_rng()) {
    send_buf_.reserve(4096);
}

WebSocketClient::~WebSocketClient() {
    if (open_) {
        close(CloseCode::kGoingAway);
    }
}

bool WebSocketClient::connected() const noexcept {
    return open_ && transport_ != nullptr && transport_->connected();
}

std::uint32_t WebSocketClient::next_mask_key() {
    // mt19937 yields 32 bits per call, which is exactly a masking key.
    return static_cast<std::uint32_t>(rng_());
}

bool WebSocketClient::connect(std::string_view url_text, int handshake_timeout_ms,
                              int read_timeout_ms) {
    error_.clear();
    reader_.reset();
    stats_ = WebSocketStats{};
    open_ = false;

    const UrlError url_status = parse_url(url_text, url_);
    if (url_status != UrlError::kOk) {
        error_ = std::string("bad url: ") + std::string(to_string(url_status));
        return false;
    }

    transport_ = make_transport(url_.secure);
    if (!transport_) {
        error_ = "no TLS backend in this build";
        return false;
    }
    if (!transport_->connect(url_.host, url_.port, handshake_timeout_ms)) {
        error_ = transport_->last_error();
        return false;
    }

    // §4.1: a fresh 16-byte nonce, base64 encoded.
    std::array<std::uint8_t, 16> nonce{};
    for (std::size_t i = 0; i < nonce.size(); i += 4) {
        const std::uint32_t bits = next_mask_key();
        std::memcpy(nonce.data() + i, &bits, 4);
    }
    const std::string key = base64_encode(nonce.data(), nonce.size());
    const std::string expected_accept = websocket_accept_for(key);

    const std::string request =
        make_handshake_request(url_.host, url_.port, url_.path, key, url_.secure);
    if (transport_->write(request.data(), request.size()) != IoStatus::kOk) {
        error_ = transport_->last_error().empty() ? "handshake write failed"
                                                  : transport_->last_error();
        transport_->close();
        return false;
    }

    if (!complete_handshake(expected_accept, handshake_timeout_ms)) {
        transport_->close();
        return false;
    }

    // The generous handshake budget has done its job; switch to a short read
    // timeout so the caller's poll loop stays responsive on a quiet market.
    transport_->set_read_timeout(read_timeout_ms);

    open_ = true;
    return true;
}

bool WebSocketClient::complete_handshake(const std::string& expected_accept, int timeout_ms) {
    (void)timeout_ms;  // The transport already carries the read timeout.

    std::string response;
    char chunk[kReadChunk];

    // Bounded so a peer that sends headers forever cannot exhaust memory before
    // it ever has to produce a valid status line.
    constexpr std::size_t kMaxHandshakeBytes = 64 * 1024;

    for (;;) {
        HandshakeResponse parsed;
        const HandshakeStatus status =
            parse_handshake_response(response, expected_accept, parsed);

        if (status == HandshakeStatus::kOk) {
            // A server may pack frames into the same segment as the 101. Those
            // bytes belong to the reader, and dropping them loses the first
            // message of the session — intermittently, which is worse.
            if (parsed.header_bytes < response.size()) {
                reader_.append(response.data() + parsed.header_bytes,
                               response.size() - parsed.header_bytes);
            }
            return true;
        }
        if (status == HandshakeStatus::kNotSwitchingProtocols) {
            error_ = "handshake rejected: HTTP " + std::to_string(parsed.status_code);
            return false;
        }
        if (status != HandshakeStatus::kIncomplete) {
            error_ = std::string("handshake failed: ") + std::string(to_string(status));
            return false;
        }

        if (response.size() > kMaxHandshakeBytes) {
            error_ = "handshake response exceeded ceiling";
            return false;
        }

        std::size_t got = 0;
        const IoStatus io = transport_->read(chunk, sizeof(chunk), got);
        if (io == IoStatus::kOk) {
            response.append(chunk, got);
            continue;
        }
        if (io == IoStatus::kTimeout) {
            error_ = "handshake timed out";
            return false;
        }
        if (io == IoStatus::kClosed) {
            error_ = "peer closed during handshake";
            return false;
        }
        error_ = transport_->last_error().empty() ? "handshake read failed"
                                                  : transport_->last_error();
        return false;
    }
}

ReadStatus WebSocketClient::poll(Event& out) {
    if (!connected()) {
        error_ = "poll: not connected";
        return ReadStatus::kProtocolError;
    }

    for (;;) {
        const ReadStatus status = reader_.next(out);

        switch (status) {
            case ReadStatus::kMessage:
                ++stats_.messages;
                return ReadStatus::kMessage;

            case ReadStatus::kPing:
                // §5.5.2: the pong must carry the ping's payload verbatim.
                ++stats_.pings_received;
                if (!send_frame(Opcode::kPong, out.payload)) {
                    return ReadStatus::kProtocolError;
                }
                ++stats_.pongs_sent;
                continue;

            case ReadStatus::kPong:
                ++stats_.pongs_received;
                continue;

            case ReadStatus::kClose:
                // Echo the code back, then stop. A client that just drops the
                // socket leaves the venue's side waiting on a half-open
                // connection until its own timeout fires.
                (void)send_frame(Opcode::kClose, out.payload);
                open_ = false;
                transport_->close();
                return ReadStatus::kClose;

            case ReadStatus::kProtocolError:
                error_ = "peer violated RFC 6455";
                close(CloseCode::kProtocolError, "protocol error");
                return ReadStatus::kProtocolError;

            case ReadStatus::kMessageTooLarge:
                error_ = "message exceeded the configured ceiling";
                close(CloseCode::kMessageTooBig, "message too big");
                return ReadStatus::kMessageTooLarge;

            case ReadStatus::kNeedMore:
                break;  // Fall through to the transport read below.
        }

        char* tail = reader_.writable_tail(kReadChunk);
        std::size_t got = 0;
        const IoStatus io = transport_->read(tail, kReadChunk, got);
        reader_.commit(io == IoStatus::kOk ? got : 0, kReadChunk);

        if (io == IoStatus::kOk) {
            stats_.bytes_received += got;
            continue;
        }
        if (io == IoStatus::kTimeout) {
            return ReadStatus::kNeedMore;
        }
        if (io == IoStatus::kClosed) {
            open_ = false;
            out.close_code = static_cast<std::uint16_t>(CloseCode::kAbnormal);
            out.payload = {};
            return ReadStatus::kClose;
        }
        error_ = transport_->last_error().empty() ? "transport read failed"
                                                  : transport_->last_error();
        open_ = false;
        return ReadStatus::kProtocolError;
    }
}

bool WebSocketClient::send_frame(Opcode opcode, std::string_view payload) {
    if (transport_ == nullptr || !transport_->connected()) {
        error_ = "send: not connected";
        return false;
    }

    const std::uint32_t mask_key = next_mask_key();

    send_buf_.clear();
    send_buf_.resize(kMaxHeaderSize + payload.size());

    const std::size_t header_size =
        write_frame_header(send_buf_.data(), opcode, payload.size(), mask_key);

    if (!payload.empty()) {
        std::memcpy(send_buf_.data() + header_size, payload.data(), payload.size());
        apply_mask(send_buf_.data() + header_size, payload.size(), mask_key);
    }

    const std::size_t total = header_size + payload.size();
    if (transport_->write(send_buf_.data(), total) != IoStatus::kOk) {
        error_ = transport_->last_error().empty() ? "frame write failed"
                                                  : transport_->last_error();
        return false;
    }
    ++stats_.frames_sent;
    return true;
}

bool WebSocketClient::send_text(std::string_view payload) {
    return send_frame(Opcode::kText, payload);
}

bool WebSocketClient::send_binary(std::string_view payload) {
    return send_frame(Opcode::kBinary, payload);
}

bool WebSocketClient::send_ping(std::string_view payload) {
    if (payload.size() > kMaxControlPayload) {
        error_ = "ping payload exceeds 125 bytes";
        return false;
    }
    return send_frame(Opcode::kPing, payload);
}

void WebSocketClient::close(CloseCode code, std::string_view reason) {
    if (transport_ != nullptr && transport_->connected() && open_) {
        // §5.5.1: two-byte big-endian code, then an optional UTF-8 reason.
        std::string payload;
        payload.reserve(2 + reason.size());
        payload.push_back(static_cast<char>((static_cast<std::uint16_t>(code) >> 8) & 0xFFU));
        payload.push_back(static_cast<char>(static_cast<std::uint16_t>(code) & 0xFFU));
        payload.append(reason.substr(0, kMaxControlPayload - 2));
        (void)send_frame(Opcode::kClose, payload);
    }
    if (transport_ != nullptr) {
        transport_->close();
    }
    open_ = false;
}

}  // namespace crossbook::net
