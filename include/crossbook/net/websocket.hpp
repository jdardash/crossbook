// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// A websocket client: transport + handshake + frame reader, assembled.
//
// This class is thin on purpose. Everything difficult about websockets lives in
// `ws_frame.hpp` and `handshake.hpp`, both of which are pure and tested without
// a network. What is left here is sequencing — dial, upgrade, verify, loop —
// plus the two obligations a client cannot delegate:
//
//   1. EVERY CLIENT FRAME MUST BE MASKED with a fresh, unpredictable key
//      (RFC 6455 §5.3). The key exists to stop a hostile page from steering a
//      proxy into caching attacker-chosen bytes; a fixed or counting key
//      defeats it entirely, so the key comes from std::random_device.
//
//   2. A PING MUST BE ANSWERED. Venues disconnect clients that do not, and the
//      resulting "the feed just stops after 60 seconds" is a miserable thing to
//      debug from the outside. `poll` answers them itself.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "crossbook/net/transport.hpp"
#include "crossbook/net/url.hpp"
#include "crossbook/net/ws_frame.hpp"

namespace crossbook::net {

/// Counters worth publishing next to a match rate: a feed that silently
/// reconnected twice during a measurement window did not measure what it claims.
struct WebSocketStats {
    std::uint64_t messages{0};
    std::uint64_t bytes_received{0};
    std::uint64_t pings_received{0};
    std::uint64_t pongs_sent{0};
    std::uint64_t pongs_received{0};
    std::uint64_t frames_sent{0};
};

class WebSocketClient {
public:
    explicit WebSocketClient(std::size_t max_message_bytes = FrameReader::kDefaultMaxMessage);
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    /// Dial `url` (ws:// or wss://) and complete the opening handshake.
    ///
    /// `timeout_ms` bounds both the connect and each subsequent read; it is not
    /// a deadline for the whole session.
    [[nodiscard]] bool connect(std::string_view url, int timeout_ms = 10'000);

    /// Next application message.
    ///
    /// - kMessage: `out.payload` is valid until the next call to `poll`.
    /// - kNeedMore: nothing arrived within the read timeout. Not an error.
    /// - kClose: the peer closed; `out.close_code` says why.
    /// - kProtocolError / kMessageTooLarge: fatal, the connection is dropped.
    ///
    /// Ping and pong frames are handled internally and never surface here.
    [[nodiscard]] ReadStatus poll(Event& out);

    [[nodiscard]] bool send_text(std::string_view payload);
    [[nodiscard]] bool send_binary(std::string_view payload);
    [[nodiscard]] bool send_ping(std::string_view payload = {});

    /// Send a close frame and drop the connection.
    void close(CloseCode code = CloseCode::kNormal, std::string_view reason = {});

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
    [[nodiscard]] const Url& url() const noexcept { return url_; }
    [[nodiscard]] const WebSocketStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] bool send_frame(Opcode opcode, std::string_view payload);
    [[nodiscard]] std::uint32_t next_mask_key();
    /// Read the handshake response, keeping any frame bytes that arrived with it.
    [[nodiscard]] bool complete_handshake(const std::string& expected_accept, int timeout_ms);

    std::unique_ptr<Transport> transport_;
    FrameReader reader_;
    Url url_;
    std::string error_;
    std::vector<char> send_buf_;
    std::mt19937 rng_;
    WebSocketStats stats_{};
    bool open_{false};
};

}  // namespace crossbook::net
