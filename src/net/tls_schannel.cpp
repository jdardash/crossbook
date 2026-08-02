// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// TLS via Schannel — the Windows backend.
//
// WHY SCHANNEL RATHER THAN OPENSSL ON WINDOWS:
//
// It ships with the operating system, so `cmake --build` produces a working
// client on a stock Windows machine with nothing installed and no vcpkg step.
// Certificate validation uses the system trust store, which is the store the
// machine's administrator actually maintains.
//
// The awkward part of Schannel is that it does not own the socket: it converts
// between ciphertext and plaintext and leaves the transport entirely to you.
// Every one of its calls can come back saying "that was not a whole record"
// (SEC_E_INCOMPLETE_MESSAGE) or "I consumed less than you gave me, the rest is
// the next record" (SECBUFFER_EXTRA). Both cases are the normal path, not the
// error path, and mishandling either produces a client that works on a fast
// local link and corrupts data the moment a record straddles two segments.
// That is the entire reason this file is longer than its OpenSSL counterpart.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define SECURITY_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// schannel.h hides the modern SCH_CREDENTIALS struct behind this macro and
// exposes only the deprecated SCHANNEL_CRED without it. SCHANNEL_CRED carries a
// protocol allow-list that this code has no business setting: pinning enabled
// protocols in application code is how a program ends up refusing TLS 1.3 years
// after the OS learned it. SCH_CREDENTIALS defers that to system policy.
#define SCHANNEL_USE_BLACKLISTS

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// subauth.h before schannel.h: with SCHANNEL_USE_BLACKLISTS defined, the
// CRYPTO_SETTINGS struct refers to PUNICODE_STRING, and nothing else in this
// include chain declares it.
#include <subauth.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>
// clang-format on

#include "crossbook/net/byte_buffer.hpp"
#include "crossbook/net/transport.hpp"
#include "tcp_socket.hpp"
#include "tls_backend.hpp"

namespace crossbook::net::detail {
namespace {

/// Ciphertext read buffer. One TLS record is at most 16 KiB of plaintext plus
/// framing; 32 KiB holds any record plus a partial successor.
constexpr std::size_t kReadChunk = 32 * 1024;

[[nodiscard]] std::string sec_error(const char* context, SECURITY_STATUS status) {
    std::string out(context);
    out.append(": SSPI status 0x");
    char hex[16];
    (void)std::snprintf(hex, sizeof(hex), "%08lX", static_cast<unsigned long>(status));
    out.append(hex);

    // The common ones by name, because "0x80090308" tells an operator nothing
    // and "certificate not trusted" tells them exactly what to go look at.
    switch (status) {
        case SEC_E_UNTRUSTED_ROOT:
            out.append(" (certificate chain not trusted)");
            break;
        case SEC_E_CERT_EXPIRED:
            out.append(" (certificate expired)");
            break;
        case SEC_E_WRONG_PRINCIPAL:
            out.append(" (certificate name mismatch)");
            break;
        case SEC_E_ILLEGAL_MESSAGE:
            out.append(" (peer sent an illegal TLS message)");
            break;
        case SEC_E_ALGORITHM_MISMATCH:
            out.append(" (no shared cipher suite)");
            break;
        default:
            break;
    }
    return out;
}

class SchannelTransport final : public Transport {
public:
    ~SchannelTransport() override { close(); }

    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port,
                               int timeout_ms) override {
        close();

        if (!socket_.connect(host, port, timeout_ms, error_)) {
            return false;
        }

        SCH_CREDENTIALS credentials{};
        credentials.dwVersion = SCH_CREDENTIALS_VERSION;
        // AUTO_CRED_VALIDATION is the default, but stating it is the point: this
        // client verifies the chain and the hostname, and a reviewer should be
        // able to see that without knowing Schannel's defaults.
        credentials.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION |
                              SCH_CRED_NO_DEFAULT_CREDS;

        TimeStamp expiry{};
        SECURITY_STATUS status = ::AcquireCredentialsHandleA(
            nullptr, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND, nullptr, &credentials,
            nullptr, nullptr, &cred_, &expiry);
        if (status != SEC_E_OK) {
            error_ = sec_error("AcquireCredentialsHandle", status);
            socket_.close();
            return false;
        }
        have_cred_ = true;

        // The name the certificate is validated against. Bracketed IPv6 literals
        // are not valid SNI, but neither is any venue addressed that way.
        target_name_ = host;
        if (!handshake()) {
            socket_.close();
            return false;
        }

        status = ::QueryContextAttributes(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
        if (status != SEC_E_OK) {
            error_ = sec_error("QueryContextAttributes(STREAM_SIZES)", status);
            socket_.close();
            return false;
        }

        send_buf_.resize(static_cast<std::size_t>(sizes_.cbHeader) +
                         static_cast<std::size_t>(sizes_.cbMaximumMessage) +
                         static_cast<std::size_t>(sizes_.cbTrailer));

        connected_ = true;
        error_.clear();
        return true;
    }

    [[nodiscard]] IoStatus read(char* buf, std::size_t len, std::size_t& n_read) override {
        if (!connected_) {
            error_ = "read: not connected";
            return IoStatus::kError;
        }
        if (len == 0) {
            n_read = 0;
            return IoStatus::kOk;
        }

        // Serve whatever a previous decrypt left over first: one TLS record can
        // hold more plaintext than the caller asked for.
        if (plain_pos_ < plain_.size()) {
            const std::size_t take = (std::min)(len, plain_.size() - plain_pos_);
            std::memcpy(buf, plain_.data() + plain_pos_, take);
            plain_pos_ += take;
            n_read = take;
            return IoStatus::kOk;
        }

        plain_.clear();
        plain_pos_ = 0;

        for (;;) {
            // Try to decrypt what is already buffered before asking for more:
            // the tail of the handshake often arrives with application data
            // behind it in the same segment.
            if (!enc_.empty()) {
                const IoStatus status = decrypt_buffered(buf, len, n_read);
                if (status == IoStatus::kOk) {
                    return IoStatus::kOk;
                }
                if (status != IoStatus::kTimeout) {
                    return status;  // kClosed or kError; kTimeout means "need more".
                }
            }

            const std::size_t old_size = enc_.size();
            enc_.resize(old_size + kReadChunk);
            std::size_t got = 0;
            const IoStatus status =
                socket_.read(enc_.data() + old_size, kReadChunk, got, error_);
            enc_.resize(old_size + (status == IoStatus::kOk ? got : 0));

            if (status == IoStatus::kOk) {
                continue;
            }
            if (status == IoStatus::kClosed) {
                connected_ = false;
            }
            if (status == IoStatus::kError) {
                connected_ = false;
            }
            return status;
        }
    }

    [[nodiscard]] IoStatus write(const char* buf, std::size_t len) override {
        if (!connected_) {
            error_ = "write: not connected";
            return IoStatus::kError;
        }

        std::size_t sent = 0;
        while (sent < len) {
            const std::size_t chunk =
                (std::min)(len - sent, static_cast<std::size_t>(sizes_.cbMaximumMessage));

            std::memcpy(send_buf_.data() + sizes_.cbHeader, buf + sent, chunk);

            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].cbBuffer = sizes_.cbHeader;
            buffers[0].pvBuffer = send_buf_.data();
            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].cbBuffer = static_cast<unsigned long>(chunk);
            buffers[1].pvBuffer = send_buf_.data() + sizes_.cbHeader;
            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].cbBuffer = sizes_.cbTrailer;
            buffers[2].pvBuffer = send_buf_.data() + sizes_.cbHeader + chunk;
            buffers[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;

            const SECURITY_STATUS status = ::EncryptMessage(&ctx_, 0, &desc, 0);
            if (status != SEC_E_OK) {
                error_ = sec_error("EncryptMessage", status);
                connected_ = false;
                return IoStatus::kError;
            }

            // EncryptMessage rewrites the buffer lengths; the record is the sum
            // of the three, which is not the same as the sum we passed in.
            const std::size_t record =
                static_cast<std::size_t>(buffers[0].cbBuffer) +
                static_cast<std::size_t>(buffers[1].cbBuffer) +
                static_cast<std::size_t>(buffers[2].cbBuffer);

            const IoStatus io = socket_.write(send_buf_.data(), record, error_);
            if (io != IoStatus::kOk) {
                connected_ = false;
                return io;
            }
            sent += chunk;
        }
        return IoStatus::kOk;
    }

    void set_read_timeout(int timeout_ms) override { socket_.set_read_timeout(timeout_ms); }

    [[nodiscard]] std::int64_t last_rx_time_ns() const noexcept override {
        return socket_.last_rx_time_ns();
    }

    void close() override {
        if (have_ctx_) {
            (void)::DeleteSecurityContext(&ctx_);
            have_ctx_ = false;
        }
        if (have_cred_) {
            (void)::FreeCredentialsHandle(&cred_);
            have_cred_ = false;
        }
        socket_.close();
        enc_.clear();
        plain_.clear();
        plain_pos_ = 0;
        connected_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }
    [[nodiscard]] const std::string& last_error() const noexcept override { return error_; }

private:
    static constexpr unsigned long kIscFlags =
        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
        ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    /// Drive InitializeSecurityContext to completion.
    ///
    /// Also used to service SEC_I_RENEGOTIATE mid-stream, which is why it starts
    /// from whatever is already in `enc_` rather than assuming an empty buffer.
    [[nodiscard]] bool handshake() {
        unsigned long out_flags = 0;
        TimeStamp expiry{};

        // --- First leg: produce ClientHello with no input. ---
        SecBuffer out_buffer{};
        out_buffer.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc out_desc{};
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buffer;

        SECURITY_STATUS status = ::InitializeSecurityContextA(
            &cred_, nullptr, target_name_.data(), kIscFlags, 0, 0, nullptr, 0, &ctx_, &out_desc,
            &out_flags, &expiry);
        if (status != SEC_I_CONTINUE_NEEDED) {
            error_ = sec_error("InitializeSecurityContext(initial)", status);
            return false;
        }
        have_ctx_ = true;

        if (out_buffer.cbBuffer != 0 && out_buffer.pvBuffer != nullptr) {
            const IoStatus io = socket_.write(static_cast<const char*>(out_buffer.pvBuffer),
                                              out_buffer.cbBuffer, error_);
            (void)::FreeContextBuffer(out_buffer.pvBuffer);
            if (io != IoStatus::kOk) {
                return false;
            }
        }

        return continue_handshake();
    }

    /// The read / InitializeSecurityContext loop, from the second leg onward.
    [[nodiscard]] bool continue_handshake() {
        unsigned long out_flags = 0;
        TimeStamp expiry{};

        // A conforming handshake is a handful of legs. The bound exists so that
        // a peer which keeps returning "continue" without consuming anything
        // cannot spin this loop forever.
        constexpr int kMaxLegs = 64;

        for (int leg = 0;; ++leg) {
            if (leg >= kMaxLegs) {
                error_ = "TLS handshake: exceeded the leg limit without completing";
                return false;
            }
            // Only read when the last attempt said the record was short. On the
            // first pass through, `enc_` is empty and this always reads.
            if (enc_.empty() || need_more_) {
                const std::size_t old_size = enc_.size();
                enc_.resize(old_size + kReadChunk);
                std::size_t got = 0;
                const IoStatus io = socket_.read(enc_.data() + old_size, kReadChunk, got, error_);
                enc_.resize(old_size + (io == IoStatus::kOk ? got : 0));
                if (io == IoStatus::kClosed) {
                    error_ = "TLS handshake: peer closed the connection";
                    return false;
                }
                if (io == IoStatus::kTimeout) {
                    error_ = "TLS handshake: timed out waiting for the server";
                    return false;
                }
                if (io == IoStatus::kError) {
                    return false;
                }
                need_more_ = false;
            }

            SecBuffer in_buffers[2]{};
            in_buffers[0].BufferType = SECBUFFER_TOKEN;
            in_buffers[0].cbBuffer = static_cast<unsigned long>(enc_.size());
            in_buffers[0].pvBuffer = enc_.data();
            in_buffers[1].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc in_desc{};
            in_desc.ulVersion = SECBUFFER_VERSION;
            in_desc.cBuffers = 2;
            in_desc.pBuffers = in_buffers;

            SecBuffer out_buffer{};
            out_buffer.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc{};
            out_desc.ulVersion = SECBUFFER_VERSION;
            out_desc.cBuffers = 1;
            out_desc.pBuffers = &out_buffer;

            const SECURITY_STATUS status = ::InitializeSecurityContextA(
                &cred_, &ctx_, target_name_.data(), kIscFlags, 0, 0, &in_desc, 0, nullptr,
                &out_desc, &out_flags, &expiry);

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                need_more_ = true;  // Keep what we have and append to it.
                continue;
            }

            if (out_buffer.cbBuffer != 0 && out_buffer.pvBuffer != nullptr) {
                const IoStatus io = socket_.write(static_cast<const char*>(out_buffer.pvBuffer),
                                                  out_buffer.cbBuffer, error_);
                (void)::FreeContextBuffer(out_buffer.pvBuffer);
                if (io != IoStatus::kOk) {
                    return false;
                }
            }

            // Whatever Schannel did not consume is the beginning of the next
            // record — possibly already application data. Dropping it here is
            // the classic Schannel bug: the first message of the session simply
            // vanishes, intermittently, depending on segmentation.
            take_extra(in_buffers[1]);

            if (status == SEC_E_OK) {
                return true;
            }
            if (status == SEC_I_CONTINUE_NEEDED) {
                need_more_ = enc_.empty();
                continue;
            }
            error_ = sec_error("InitializeSecurityContext", status);
            return false;
        }
    }

    /// Move a SECBUFFER_EXTRA span to the front of the ciphertext buffer.
    ///
    /// The two producers describe the span differently, which is a documented
    /// asymmetry and an easy thing to get wrong in one of the two places:
    /// InitializeSecurityContext reports only `cbBuffer`, counted back from the
    /// end of the input, while DecryptMessage also fills in `pvBuffer`.
    void take_extra(const SecBuffer& extra) {
        if (extra.BufferType != SECBUFFER_EXTRA || extra.cbBuffer == 0) {
            enc_.clear();
            return;
        }
        const std::size_t count = static_cast<std::size_t>(extra.cbBuffer);
        if (count > enc_.size()) {
            enc_.clear();  // Cannot happen; not worth trusting that it cannot.
            return;
        }
        const char* src = (extra.pvBuffer != nullptr)
                              ? static_cast<const char*>(extra.pvBuffer)
                              : enc_.data() + (enc_.size() - count);
        std::memmove(enc_.data(), src, count);
        enc_.resize(count);
    }

    /// Decrypt one record out of `enc_`, straight into the caller's buffer.
    ///
    /// DecryptMessage already decrypts in place inside `enc_`, so the only
    /// copy this path needs is the one into `dst`. Plaintext beyond `len`
    /// goes to `plain_` for the next call to serve; with a 16 KiB TLS record
    /// ceiling and the frame reader asking for 32 KiB chunks, that overflow
    /// path is reachable only for callers reading less than a record.
    ///
    /// Returns kTimeout to mean "incomplete record, read more" — the caller's
    /// loop treats it as such, and no other status can express it.
    [[nodiscard]] IoStatus decrypt_buffered(char* dst, std::size_t len, std::size_t& n_read) {
        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].cbBuffer = static_cast<unsigned long>(enc_.size());
        buffers[0].pvBuffer = enc_.data();
        buffers[1].BufferType = SECBUFFER_EMPTY;
        buffers[2].BufferType = SECBUFFER_EMPTY;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc{};
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        const SECURITY_STATUS status = ::DecryptMessage(&ctx_, &desc, 0, nullptr);

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            return IoStatus::kTimeout;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) {
            connected_ = false;
            return IoStatus::kClosed;  // The peer sent close_notify.
        }
        if (status == SEC_I_RENEGOTIATE) {
            // Rare on TLS 1.2 and absent on 1.3, but a server may ask. Resuming
            // the handshake loop is the whole handling; the alternative is
            // dropping the connection on a legal server request.
            const SecBuffer* extra = find_buffer(buffers, SECBUFFER_EXTRA);
            if (extra != nullptr && extra->cbBuffer != 0) {
                take_extra(*extra);
            } else {
                enc_.clear();
            }
            need_more_ = enc_.empty();
            if (!continue_handshake()) {
                connected_ = false;
                return IoStatus::kError;
            }
            return IoStatus::kTimeout;  // Nothing decrypted yet; loop again.
        }
        if (status != SEC_E_OK) {
            error_ = sec_error("DecryptMessage", status);
            connected_ = false;
            return IoStatus::kError;
        }

        // Copy plaintext out BEFORE touching the ciphertext buffer: both the
        // data span and the extra span point into `enc_` itself, and
        // `take_extra`'s memmove overwrites the front of it.
        std::size_t produced = 0;
        const SecBuffer* data = find_buffer(buffers, SECBUFFER_DATA);
        if (data != nullptr && data->cbBuffer != 0) {
            const char* p = static_cast<const char*>(data->pvBuffer);
            const std::size_t cb = data->cbBuffer;
            produced = (std::min)(len, cb);
            std::memcpy(dst, p, produced);
            if (cb > produced) {
                plain_.assign(p + produced, p + cb);
                plain_pos_ = 0;
            }
        }

        // take_extra memmoves the unconsumed tail to the front of `enc_` in
        // place — a fresh vector per pipelined record was an allocation on
        // the common path, since a busy feed routinely lands the next record
        // behind the current one in the same segment.
        const SecBuffer* extra = find_buffer(buffers, SECBUFFER_EXTRA);
        if (extra != nullptr) {
            take_extra(*extra);
        } else {
            enc_.clear();
        }

        if (produced == 0) {
            return IoStatus::kTimeout;  // A record with no app data; loop.
        }
        n_read = produced;
        return IoStatus::kOk;
    }

    /// First buffer of a given type, skipping the one we handed in as input.
    [[nodiscard]] static const SecBuffer* find_buffer(const SecBuffer (&buffers)[4],
                                                      unsigned long type) noexcept {
        for (std::size_t i = 1; i < 4; ++i) {
            if (buffers[i].BufferType == type) {
                return &buffers[i];
            }
        }
        return nullptr;
    }

    TcpSocket socket_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    SecPkgContext_StreamSizes sizes_{};
    std::string target_name_;
    // ByteBuffer, not std::vector<char>: the read loops grow this by a 32 KiB
    // chunk per socket read and trim it back to what arrived, and a
    // value-initializing resize would memset the chunk every time.
    ByteBuffer enc_;
    std::vector<char> plain_;
    std::vector<char> send_buf_;
    std::string error_;
    std::size_t plain_pos_{0};
    bool have_cred_{false};
    bool have_ctx_{false};
    bool need_more_{false};
    bool connected_{false};
};

}  // namespace

std::unique_ptr<Transport> make_tls_transport() { return std::make_unique<SchannelTransport>(); }

}  // namespace crossbook::net::detail
