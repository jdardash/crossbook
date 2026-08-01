// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// An assertion that survives NDEBUG.
//
// Fuzz targets are built RelWithDebInfo, because fuzzing an unoptimised binary
// wastes most of the CPU budget. RelWithDebInfo defines NDEBUG, which makes
// <cassert>'s assert() expand to nothing.
//
// That combination is a trap, and this library walked into it: the first
// version of these targets used assert(), so every correctness oracle was
// silently compiled away and the fuzzers were reduced to crash detectors. They
// would have run green forever while checking nothing. The compiler noticed
// only because the now-unreferenced variables tripped -Wunused-variable.
//
// CB_CHECK is unconditional. It cannot be disabled by a build type.

#pragma once

#include <cstdio>
#include <cstdlib>

#define CB_CHECK(cond)                                                                  \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "CB_CHECK failed: %s\n  at %s:%d\n", #cond, __FILE__,  \
                         __LINE__);                                                     \
            std::abort();                                                               \
        }                                                                               \
    } while (0)
