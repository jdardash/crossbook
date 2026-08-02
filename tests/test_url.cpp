// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include "crossbook/net/url.hpp"

using namespace crossbook::net;

TEST_CASE("Websocket URLs parse with the scheme's default port") {
    Url url;
    REQUIRE(parse_url("wss://ws.kraken.com/v2", url) == UrlError::kOk);
    CHECK(url.secure);
    CHECK(url.host == "ws.kraken.com");
    CHECK(url.port == 443);
    CHECK(url.path == "/v2");

    REQUIRE(parse_url("ws://localhost/feed", url) == UrlError::kOk);
    CHECK_FALSE(url.secure);
    CHECK(url.port == 80);
}

TEST_CASE("An explicit port overrides the default") {
    Url url;
    REQUIRE(parse_url("wss://stream.binance.com:9443/ws/btcusdt@depth@100ms", url) ==
            UrlError::kOk);
    CHECK(url.host == "stream.binance.com");
    CHECK(url.port == 9443);
    CHECK(url.path == "/ws/btcusdt@depth@100ms");
}

TEST_CASE("A missing path becomes the root") {
    Url url;
    REQUIRE(parse_url("wss://example.com", url) == UrlError::kOk);
    CHECK(url.path == "/");
}

TEST_CASE("IPv6 literals are not split on their own colons") {
    Url url;
    REQUIRE(parse_url("ws://[2001:db8::1]:8080/x", url) == UrlError::kOk);
    CHECK(url.host == "[2001:db8::1]");
    CHECK(url.port == 8080);
    CHECK(url.path == "/x");

    REQUIRE(parse_url("wss://[2001:db8::1]/", url) == UrlError::kOk);
    CHECK(url.host == "[2001:db8::1]");
    CHECK(url.port == 443);
}

TEST_CASE("Anything that is not ws or wss is refused") {
    Url url;
    // https:// in particular: a plausible typo that must not silently become a
    // connection attempt against a port nobody meant.
    CHECK(parse_url("https://example.com", url) == UrlError::kBadScheme);
    CHECK(parse_url("example.com", url) == UrlError::kBadScheme);
    CHECK(parse_url("", url) == UrlError::kBadScheme);
}

TEST_CASE("Malformed ports are refused rather than defaulted") {
    Url url;
    CHECK(parse_url("wss://example.com:0/", url) == UrlError::kBadPort);
    CHECK(parse_url("wss://example.com:99999/", url) == UrlError::kBadPort);
    CHECK(parse_url("wss://example.com:abc/", url) == UrlError::kBadPort);
    CHECK(parse_url("wss://example.com:/", url) == UrlError::kBadPort);
}

TEST_CASE("An empty host is refused") {
    Url url;
    CHECK(parse_url("wss:///path", url) == UrlError::kEmptyHost);
}
