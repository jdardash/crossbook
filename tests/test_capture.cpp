// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

#include "crossbook/capture.hpp"

using namespace crossbook;

namespace {

std::string temp_path(const char* name) { return std::string("cbtest_") + name + ".cbcap"; }

struct TempFile {
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
    std::string path;
};

}  // namespace

TEST_CASE("a written capture reads back byte for byte", "[capture]") {
    // Payloads are stored verbatim with no normalisation, because the checksum
    // depends on the exact number spelling and whitespace the wire carried.
    TempFile file(temp_path("roundtrip"));

    CaptureWriter writer;
    REQUIRE(writer.open(file.path));
    REQUIRE(writer.write(1000, R"({"price":45285.2,"qty":0.00100000})"));
    REQUIRE(writer.write(2000, R"({"a": [1,  2] })"));
    REQUIRE(writer.write(3500, ""));
    writer.close();
    CHECK(writer.records() == 3);

    Capture capture;
    REQUIRE(capture.load(file.path) == CaptureError::kOk);
    REQUIRE(capture.size() == 3);
    CHECK(capture.events()[0].ts_recv == 1000);
    CHECK(capture.events()[0].frame == R"({"price":45285.2,"qty":0.00100000})");
    CHECK(capture.events()[1].frame == R"({"a": [1,  2] })");
    CHECK(capture.events()[2].frame.empty());
    CHECK(capture.duration_ns() == 2500);
}

TEST_CASE("a missing file is reported", "[capture]") {
    Capture capture;
    CHECK(capture.load("definitely_not_here.cbcap") == CaptureError::kNotFound);
}

TEST_CASE("a foreign file is rejected on magic", "[capture]") {
    TempFile file(temp_path("magic"));
    {
        std::ofstream out(file.path, std::ios::binary);
        out << "this is not a capture file at all";
    }
    Capture capture;
    CHECK(capture.load(file.path) == CaptureError::kBadMagic);
}

TEST_CASE("a truncated capture is rejected rather than partly decoded", "[capture]") {
    // A capture cut off mid-record would otherwise hand a partial frame to the
    // decoder, and "the last frame was garbage" is not something a verification
    // run should discover as a divergence. This is exactly what a live-growing
    // file looks like.
    TempFile full(temp_path("full"));
    CaptureWriter writer;
    REQUIRE(writer.open(full.path));
    REQUIRE(writer.write(1000, "a complete frame payload"));
    writer.close();

    std::string bytes;
    {
        std::ifstream in(full.path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > 12);

    TempFile cut(temp_path("cut"));
    for (std::size_t keep : {bytes.size() - 1, bytes.size() - 5, static_cast<std::size_t>(10)}) {
        {
            std::ofstream out(cut.path, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(keep));
        }
        INFO("kept " << keep << " of " << bytes.size() << " bytes");
        Capture capture;
        CHECK(capture.load(cut.path) == CaptureError::kTruncated);
    }
}

TEST_CASE("an implausible record length is refused, not allocated", "[capture]") {
    // A corrupt capture must not be able to exhaust memory.
    TempFile file(temp_path("huge"));
    {
        std::ofstream out(file.path, std::ios::binary);
        out.write(kCaptureMagic, sizeof(kCaptureMagic));
        const std::int64_t ts = 1000;
        const std::uint32_t length = 0xFFFFFFFFu;
        out.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
        out.write(reinterpret_cast<const char*>(&length), sizeof(length));
    }
    Capture capture;
    CHECK(capture.load(file.path) == CaptureError::kImplausibleRecord);
}

TEST_CASE("an empty capture is valid but empty", "[capture]") {
    TempFile file(temp_path("empty"));
    CaptureWriter writer;
    REQUIRE(writer.open(file.path));
    writer.close();

    Capture capture;
    REQUIRE(capture.load(file.path) == CaptureError::kOk);
    CHECK(capture.empty());
    CHECK(capture.duration_ns() == 0);
}

TEST_CASE("frame views survive buffer growth during load", "[capture]") {
    // The payload buffer reallocates as records are read; views taken during
    // the read would dangle. Enough records to force several reallocations.
    TempFile file(temp_path("growth"));
    CaptureWriter writer;
    REQUIRE(writer.open(file.path));
    for (int i = 0; i < 2000; ++i) {
        REQUIRE(writer.write(i * 1000, std::string(200, static_cast<char>('a' + (i % 26)))));
    }
    writer.close();

    Capture capture;
    REQUIRE(capture.load(file.path) == CaptureError::kOk);
    REQUIRE(capture.size() == 2000);
    for (int i = 0; i < 2000; ++i) {
        const std::string_view frame = capture.events()[static_cast<std::size_t>(i)].frame;
        REQUIRE(frame.size() == 200);
        CHECK(frame[0] == static_cast<char>('a' + (i % 26)));
    }
}
