// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Capture files.
//
// The format exists so a live measurement can be re-run by someone else, which
// only works if the bytes survive the round trip exactly. Every test here is
// ultimately asking the same question: are these the same bytes the venue sent?

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "crossbook/capture.hpp"

using namespace crossbook;

TEST_CASE("Frames round-trip through a capture unchanged") {
    Capture capture;
    std::string error;

    const std::string contents =
        "CBCAP1 kraken BTC/USD 1700000000000000000\n"
        "1000 5\n"
        "hello\n"
        "2500 3\n"
        "abc\n";

    REQUIRE(capture.parse(contents, error));
    CHECK(capture.venue() == "kraken");
    CHECK(capture.symbol() == "BTC/USD");
    CHECK(capture.start_unix_ns() == 1'700'000'000'000'000'000LL);

    REQUIRE(capture.frames().size() == 2);
    CHECK(capture.frames()[0].ts_recv == 1000);
    CHECK(capture.frames()[0].payload == "hello");
    CHECK(capture.frames()[1].ts_recv == 2500);
    CHECK(capture.frames()[1].payload == "abc");
}

TEST_CASE("A payload containing a newline stays one frame") {
    // The reason the format is length-prefixed rather than line-delimited. A
    // venue is entitled to put a newline inside a JSON string, and a
    // line-oriented reader would turn one frame into two and corrupt the book
    // replayed from it.
    Capture capture;
    std::string error;

    const std::string payload = "{\"a\":\"line1\nline2\"}";
    const std::string contents = "CBCAP1 test SYM 0\n1 " + std::to_string(payload.size()) + "\n" +
                                 payload + "\n";

    REQUIRE(capture.parse(contents, error));
    REQUIRE(capture.frames().size() == 1);
    CHECK(capture.frames()[0].payload == payload);
}

TEST_CASE("An empty frame is preserved") {
    Capture capture;
    std::string error;
    REQUIRE(capture.parse("CBCAP1 test SYM 0\n1 0\n\n", error));
    REQUIRE(capture.frames().size() == 1);
    CHECK(capture.frames()[0].payload.empty());
}

TEST_CASE("A capture cut short mid-frame loads as its valid prefix") {
    // Ctrl-C during recording leaves exactly this. Refusing to load it would
    // throw away every interrupted run, which is most of them.
    Capture capture;
    std::string error;

    const std::string contents =
        "CBCAP1 kraken BTC/USD 0\n"
        "1 5\n"
        "hello\n"
        "2 100\n"
        "trunc";

    REQUIRE(capture.parse(contents, error));
    REQUIRE(capture.frames().size() == 1);
    CHECK(capture.frames()[0].payload == "hello");
}

TEST_CASE("A file that is not a capture is refused") {
    Capture capture;
    std::string error;

    CHECK_FALSE(capture.parse("", error));
    CHECK_FALSE(capture.parse("not a capture\n1 2\nab\n", error));
    CHECK_FALSE(capture.parse("CBCAP1\n", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("A malformed record header is an error, not a silent stop") {
    Capture capture;
    std::string error;
    CHECK_FALSE(capture.parse("CBCAP1 v s 0\nnotanumber\nxx\n", error));
}

TEST_CASE("The writer produces something the reader accepts") {
    const std::string path =
        (std::string(CROSSBOOK_TEST_TMP_DIR) + "/crossbook_capture_roundtrip.cbcap");

    {
        CaptureWriter writer;
        REQUIRE(writer.open(path, "kraken", "BTC/USD", 1234));
        REQUIRE(writer.write(10, R"({"channel":"book","type":"snapshot"})"));
        REQUIRE(writer.write(20, R"({"channel":"book","type":"update"})"));
        REQUIRE(writer.write(30, ""));
        CHECK(writer.frames() == 3);
    }

    Capture capture;
    std::string error;
    REQUIRE(capture.load(path, error));
    CHECK(capture.venue() == "kraken");
    CHECK(capture.symbol() == "BTC/USD");
    CHECK(capture.start_unix_ns() == 1234);
    REQUIRE(capture.frames().size() == 3);
    CHECK(capture.frames()[0].payload == R"({"channel":"book","type":"snapshot"})");
    CHECK(capture.frames()[1].payload == R"({"channel":"book","type":"update"})");
    CHECK(capture.frames()[2].payload.empty());
    CHECK(capture.frames()[2].ts_recv == 30);
}

TEST_CASE("The median gap ignores the bursts a mean would follow") {
    Capture capture;
    std::string error;

    // Four fast arrivals and one long pause. The mean gap is dominated by the
    // pause; the median describes what the feed actually does most of the time.
    REQUIRE(capture.parse(
        "CBCAP1 v s 0\n"
        "0 1\na\n"
        "100 1\nb\n"
        "200 1\nc\n"
        "300 1\nd\n"
        "1000300 1\ne\n",
        error));

    CHECK(capture.frames().size() == 5);
    CHECK(capture.median_gap_ns() == 100);
}

TEST_CASE("A capture of one frame has no gap to report") {
    Capture capture;
    std::string error;
    REQUIRE(capture.parse("CBCAP1 v s 0\n5 1\na\n", error));
    CHECK(capture.median_gap_ns() == 0);
}
