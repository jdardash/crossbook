// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// TLS via OpenSSL — the POSIX backend.
//
// Shorter than its Schannel counterpart for one reason: OpenSSL will own the
// file descriptor and do its own reading, so there is no ciphertext buffer to
// manage and no SECBUFFER_EXTRA to get wrong.
//
// HOSTNAME VERIFICATION IS EXPLICIT. `SSL_CTX_set_verify` alone validates the
// chain but not the name, so a certificate legitimately issued for any host at
// all would pass. `SSL_set1_host` is what ties the connection to the host we
// meant to reach, and it is the line most often missing from hand-rolled
// OpenSSL clients.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "crossbook/net/transport.hpp"
#include "tcp_socket.hpp"
#include "tls_backend.hpp"

namespace crossbook::net::detail {
namespace {

[[nodiscard]] std::string openssl_error(const char* context) {
    std::string out(context);
    out.append(": ");
    const unsigned long code = ::ERR_get_error();
    if (code == 0) {
        out.append("no OpenSSL error queued");
        return out;
    }
    char buf[256];
    ::ERR_error_string_n(code, buf, sizeof(buf));
    out.append(buf);
    return out;
}

class OpenSslTransport final : public Transport {
public:
    ~OpenSslTransport() override { close(); }

    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port,
                               int timeout_ms) override {
        close();

        if (!socket_.connect(host, port, timeout_ms, error_)) {
            return false;
        }

        ctx_ = ::SSL_CTX_new(::TLS_client_method());
        if (ctx_ == nullptr) {
            error_ = openssl_error("SSL_CTX_new");
            socket_.close();
            return false;
        }

        // TLS 1.0 and 1.1 are dead; refusing them here means a downgrade cannot
        // be negotiated on our side at all.
        (void)::SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        ::SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        if (::SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            error_ = openssl_error("SSL_CTX_set_default_verify_paths");
            close();
            return false;
        }

        ssl_ = ::SSL_new(ctx_);
        if (ssl_ == nullptr) {
            error_ = openssl_error("SSL_new");
            close();
            return false;
        }

        // Strip brackets from an IPv6 literal before using it as a name.
        std::string name = host;
        if (name.size() >= 2 && name.front() == '[' && name.back() == ']') {
            name = name.substr(1, name.size() - 2);
        }

        // SNI: without it a venue behind shared hosting serves the wrong
        // certificate and the handshake fails for a reason that looks like ours.
        if (::SSL_set_tlsext_host_name(ssl_, name.c_str()) != 1) {
            error_ = openssl_error("SSL_set_tlsext_host_name");
            close();
            return false;
        }
        // Chain validation plus name validation; see the file header.
        if (::SSL_set1_host(ssl_, name.c_str()) != 1) {
            error_ = openssl_error("SSL_set1_host");
            close();
            return false;
        }

        if (::SSL_set_fd(ssl_, static_cast<int>(socket_.handle())) != 1) {
            error_ = openssl_error("SSL_set_fd");
            close();
            return false;
        }

        const int rc = ::SSL_connect(ssl_);
        if (rc != 1) {
            const long verify = ::SSL_get_verify_result(ssl_);
            if (verify != X509_V_OK) {
                error_ = std::string("TLS handshake: certificate rejected: ") +
                         ::X509_verify_cert_error_string(verify);
            } else {
                error_ = openssl_error("SSL_connect");
            }
            close();
            return false;
        }

        connected_ = true;
        error_.clear();
        return true;
    }

    [[nodiscard]] IoStatus read(char* buf, std::size_t len, std::size_t& n_read) override {
        if (!connected_ || ssl_ == nullptr) {
            error_ = "read: not connected";
            return IoStatus::kError;
        }
        if (len == 0) {
            n_read = 0;
            return IoStatus::kOk;
        }

        ::ERR_clear_error();
        const int got = ::SSL_read(ssl_, buf, static_cast<int>(std::min<std::size_t>(
                                                 len, static_cast<std::size_t>(INT32_MAX))));
        if (got > 0) {
            n_read = static_cast<std::size_t>(got);
            return IoStatus::kOk;
        }
        return classify(got);
    }

    [[nodiscard]] IoStatus write(const char* buf, std::size_t len) override {
        if (!connected_ || ssl_ == nullptr) {
            error_ = "write: not connected";
            return IoStatus::kError;
        }

        std::size_t sent = 0;
        while (sent < len) {
            ::ERR_clear_error();
            const int n = ::SSL_write(
                ssl_, buf + sent,
                static_cast<int>(std::min<std::size_t>(len - sent,
                                                       static_cast<std::size_t>(INT32_MAX))));
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            return classify(n);
        }
        return IoStatus::kOk;
    }

    void set_read_timeout(int timeout_ms) override { socket_.set_read_timeout(timeout_ms); }

    void close() override {
        if (ssl_ != nullptr) {
            // Best-effort close_notify. A venue that has already gone away makes
            // this fail, which is not worth reporting.
            (void)::SSL_shutdown(ssl_);
            ::SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr) {
            ::SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        socket_.close();
        connected_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }
    [[nodiscard]] const std::string& last_error() const noexcept override { return error_; }

private:
    /// Map a non-positive SSL_read / SSL_write return onto our status.
    ///
    /// The socket is blocking with a timeout, so an expired timeout surfaces as
    /// WANT_READ / WANT_WRITE rather than as an error. Treating those as
    /// failures would tear down the connection every time the market went quiet.
    [[nodiscard]] IoStatus classify(int rc) {
        const int err = ::SSL_get_error(ssl_, rc);
        switch (err) {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                return IoStatus::kTimeout;
            case SSL_ERROR_ZERO_RETURN:
                connected_ = false;
                return IoStatus::kClosed;
            case SSL_ERROR_SYSCALL:
                if (rc == 0) {
                    connected_ = false;
                    return IoStatus::kClosed;  // Clean EOF without close_notify.
                }
                connected_ = false;
                error_ = socket_error_string("SSL_ERROR_SYSCALL");
                return IoStatus::kError;
            default:
                connected_ = false;
                error_ = openssl_error("SSL");
                return IoStatus::kError;
        }
    }

    TcpSocket socket_;
    ::SSL_CTX* ctx_{nullptr};
    ::SSL* ssl_{nullptr};
    std::string error_;
    bool connected_{false};
};

}  // namespace

std::unique_ptr<Transport> make_tls_transport() { return std::make_unique<OpenSslTransport>(); }

}  // namespace crossbook::net::detail
