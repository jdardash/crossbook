// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// A byte buffer whose resize does not zero.
//
// std::vector<char>::resize value-initializes every new element, which for a
// receive buffer means a memset over bytes the transport is about to
// overwrite anyway. The reassembly path grows its buffer by 32 KiB on every
// socket read and then shrinks it back to what was actually received; with a
// plain vector that is 32 KiB of zeroing per read, all of it wasted.
//
// The standard fix: an allocator whose construct() default-initializes
// instead of value-initializing. For trivially default-constructible types,
// default-initialization is a no-op, so resize becomes pure bookkeeping.
// Everything else about std::vector — growth policy, iterator semantics,
// exception guarantees — is unchanged.

#pragma once

#include <memory>
#include <utility>
#include <vector>

namespace crossbook::net::detail {

template <typename T, typename Base = std::allocator<T>>
class DefaultInitAllocator : public Base {
public:
    template <typename U>
    struct rebind {
        using other =
            DefaultInitAllocator<U, typename std::allocator_traits<Base>::template rebind_alloc<U>>;
    };

    using Base::Base;

    /// The point of the class: `new (p) U` default-initializes, so for byte
    /// buffers no memory is written until the caller writes it.
    template <typename U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(p)) U;
    }

    /// Constructions with arguments (insert, push_back, range copies) keep
    /// their ordinary value semantics.
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        std::allocator_traits<Base>::construct(static_cast<Base&>(*this), p,
                                               std::forward<Args>(args)...);
    }
};

}  // namespace crossbook::net::detail

namespace crossbook::net {

/// Receive-path byte storage: a std::vector<char> whose resize is free.
using ByteBuffer = std::vector<char, detail::DefaultInitAllocator<char>>;

}  // namespace crossbook::net
