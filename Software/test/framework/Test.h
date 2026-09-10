/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

// Minimal, dependency-free xUnit-style harness for the host test build.
// No heap, no STL, no <sstream> (Lib/Tools/SStream.h defines its own
// std::stringstream and must stay the only one in a translation unit).
//
//   CC_TEST(Suite, Name) { CC_CHECK(cond); CC_CHECK_EQ(a, b); }
//
// main() lives in Test.cpp; a test executable only links this and writes
// CC_TEST blocks. Exit code is non-zero if any check failed.

#include <stdint.h>

namespace cctest
{
    using TestFn = void (*)();

    // File-scope objects of this type self-register a test on construction.
    struct Registrar
    {
        Registrar(const char* suite, const char* name, TestFn fn);
    };

    void Fail(const char* file, int line, const char* expr);
    void FailEq(const char* file, int line, const char* expr, long long lhs, long long rhs);

    int RunAll();

    namespace detail
    {
        inline long long ToLL(long long value)
        {
            return value;
        }

        template <typename T>
        inline long long ToLL(T value)
        {
            return static_cast<long long>(value);
        }
    } // namespace detail
} // namespace cctest

#define CC_TEST(suite, name)                                                                      \
    static void                cc_test_##suite##_##name();                                        \
    static ::cctest::Registrar cc_reg_##suite##_##name(#suite, #name, &cc_test_##suite##_##name); \
    static void                cc_test_##suite##_##name()

#define CC_CHECK(cond)                                 \
    do                                                 \
    {                                                  \
        if (!(cond))                                   \
        {                                              \
            ::cctest::Fail(__FILE__, __LINE__, #cond); \
        }                                              \
    } while (0)

#define CC_CHECK_EQ(a, b)                                                     \
    do                                                                        \
    {                                                                         \
        const long long ccLhs = ::cctest::detail::ToLL(a);                    \
        const long long ccRhs = ::cctest::detail::ToLL(b);                    \
        if (ccLhs != ccRhs)                                                   \
        {                                                                     \
            ::cctest::FailEq(__FILE__, __LINE__, #a " == " #b, ccLhs, ccRhs); \
        }                                                                     \
    } while (0)
