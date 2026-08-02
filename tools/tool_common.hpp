// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Shared plumbing for the command-line tools.
//
// Deliberately not part of the library: this is operational policy — how long
// to wait, how many times to try — and policy belongs to the program making the
// decision, not to the protocol implementation underneath it.

#pragma once

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "crossbook/net/websocket.hpp"

namespace crossbook::tools {

/// Connect, retrying with exponential backoff.
///
/// Not defensive padding. Venues throttle repeated connections from one address
/// — reconnecting in a tight loop after a disconnect is the behaviour that earns
/// the throttle in the first place — and a stall during the opening handshake is
/// what that throttling looks like from the client side. Backing off is the
/// documented way to behave, and a verifier that gives up on the first refused
/// connection cannot be pointed at a long run.
[[nodiscard]] inline bool connect_with_backoff(net::WebSocketClient& client,
                                               const std::string& url, int attempts = 4) {
    int delay_ms = 1000;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (client.connect(url)) {
            return true;
        }
        if (attempt == attempts) {
            break;
        }
        std::fprintf(stderr, "connect attempt %d/%d failed (%s); retrying in %.1fs\n", attempt,
                     attempts, client.last_error().c_str(), delay_ms / 1000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        delay_ms *= 2;
    }
    return false;
}

}  // namespace crossbook::tools
