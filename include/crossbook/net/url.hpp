// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Just enough URL parsing to dial a websocket endpoint.
//
// Deliberately not a general URL parser. It accepts `ws://` and `wss://` with
// an optional port and path, and rejects everything else rather than guessing.
// A feed handler dials a handful of endpoints from configuration; the failure
// mode worth engineering against is a typo silently becoming a connection to
// the wrong host, not an inability to parse userinfo and fragments.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace crossbook::net {

struct Url {
    std::string host;
    std::string path{"/"};
    std::uint16_t port{0};
    bool secure{false};
};

enum class UrlError : std::uint8_t {
    kOk,
    /// Scheme was absent or was something other than ws / wss.
    kBadScheme,
    kEmptyHost,
    /// Port was not a decimal number in [1, 65535].
    kBadPort,
};

[[nodiscard]] constexpr std::string_view to_string(UrlError e) noexcept {
    switch (e) {
        case UrlError::kOk:
            return "ok";
        case UrlError::kBadScheme:
            return "bad_scheme";
        case UrlError::kEmptyHost:
            return "empty_host";
        case UrlError::kBadPort:
            return "bad_port";
    }
    return "unknown";
}

/// Decimal port, rejecting empty input, non-digits, and anything out of range.
/// Separate so the overflow check is stated once rather than inlined twice.
[[nodiscard]] inline bool parse_port(std::string_view text, std::uint16_t& out) noexcept {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint32_t>(c - '0');
    }
    if (value == 0 || value > 65535U) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

/// Parse `ws://host[:port][/path]` or `wss://...`.
///
/// The default port follows the scheme — 80 for ws, 443 for wss — which is the
/// one piece of implicit behaviour here, and it is the one every venue relies
/// on.
[[nodiscard]] inline UrlError parse_url(std::string_view text, Url& out) {
    Url url;

    constexpr std::string_view kSecurePrefix = "wss://";
    constexpr std::string_view kPlainPrefix = "ws://";

    if (text.starts_with(kSecurePrefix)) {
        url.secure = true;
        url.port = 443;
        text.remove_prefix(kSecurePrefix.size());
    } else if (text.starts_with(kPlainPrefix)) {
        url.secure = false;
        url.port = 80;
        text.remove_prefix(kPlainPrefix.size());
    } else {
        return UrlError::kBadScheme;
    }

    // Split authority from path at the first '/'.
    std::string_view authority = text;
    const std::size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        authority = text.substr(0, slash);
        url.path = std::string(text.substr(slash));
    }

    // Split host from port at the last ':', so an IPv6 literal in brackets is
    // not mangled by the colons inside it.
    std::string_view host = authority;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return UrlError::kEmptyHost;
        }
        host = authority.substr(0, close + 1);
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return UrlError::kBadPort;
            }
            if (!parse_port(rest.substr(1), url.port)) {
                return UrlError::kBadPort;
            }
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            if (!parse_port(authority.substr(colon + 1), url.port)) {
                return UrlError::kBadPort;
            }
        }
    }

    if (host.empty()) {
        return UrlError::kEmptyHost;
    }
    url.host = std::string(host);

    out = std::move(url);
    return UrlError::kOk;
}

}  // namespace crossbook::net
