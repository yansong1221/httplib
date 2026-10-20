#pragma once

// Compile-time mirrors of the optional CMake features, so a test that exercises an
// unbuilt feature SKIPs instead of failing. The feature macros are PRIVATE compile
// definitions shared by the library and the in-tree test targets; they never reach
// the exported/public surface.
//
// Prefer SKIP() over #ifdef: it keeps the test case registered and visible, so a run
// reports "skipped: needs HTTPLIB_ENABLED_COMPRESS" rather than silently disappearing.
// A silently-absent test looks exactly like a passing one.

#include "httplib/config.hpp"

#ifdef HTTPLIB_ENABLED_SSL
#define HTTPLIB_TEST_SSL 1
#else
#define HTTPLIB_TEST_SSL 0
#endif

#ifdef HTTPLIB_ENABLED_COMPRESS
#define HTTPLIB_TEST_COMPRESS 1
#else
#define HTTPLIB_TEST_COMPRESS 0
#endif

#ifdef HTTPLIB_ENABLED_DATABASE
#define HTTPLIB_TEST_DATABASE 1
#else
#define HTTPLIB_TEST_DATABASE 0
#endif

#include <catch2/catch_test_macros.hpp>

#define SKIP_WITHOUT_SSL()                          \
    do {                                            \
        if (!HTTPLIB_TEST_SSL)                      \
        {                                           \
            SKIP("requires HTTPLIB_ENABLED_SSL");   \
        }                                           \
    } while (false)

#define SKIP_WITHOUT_COMPRESS()                          \
    do {                                                 \
        if (!HTTPLIB_TEST_COMPRESS)                      \
        {                                                \
            SKIP("requires HTTPLIB_ENABLED_COMPRESS");   \
        }                                                \
    } while (false)
