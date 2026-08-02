// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Internal: the seam between the transport factory and whichever TLS backend
// this platform was built with. Exactly one translation unit defines it.

#pragma once

#include <memory>

#include "crossbook/net/transport.hpp"

namespace crossbook::net::detail {

/// Defined by tls_schannel.cpp on Windows and tls_openssl.cpp elsewhere.
[[nodiscard]] std::unique_ptr<Transport> make_tls_transport();

}  // namespace crossbook::net::detail
