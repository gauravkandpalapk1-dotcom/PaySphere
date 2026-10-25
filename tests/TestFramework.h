#pragma once

// Minimal self-registering test harness, so the project needs no external
// test framework download.

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pstest {

struct TestCase {
    std::string name;
    void (*body)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*body)()) { registry().push_back({name, body}); }
};

class AssertionFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    std::ostringstream out;
    out << file << ':' << line << ": " << message;
    throw AssertionFailure(out.str());
}

} // namespace pstest

#define TEST(name)                                                        \
    static void name();                                                   \
    static const ::pstest::Registrar name##_registrar(#name, &name);      \
    static void name()

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            ::pstest::fail(__FILE__, __LINE__, "CHECK(" #condition ")");  \
        }                                                                 \
    } while (false)

// Copies both values: holding references could dangle, e.g. for
// queryInt64(...).value(), which refers into a temporary optional.
#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        const auto actual_ = (actual);                                    \
        const auto expected_ = (expected);                                \
        if (!(actual_ == expected_)) {                                    \
            std::ostringstream message_;                                  \
            message_ << "CHECK_EQ(" #actual ", " #expected "): got "      \
                     << actual_ << ", expected " << expected_;            \
            ::pstest::fail(__FILE__, __LINE__, message_.str());           \
        }                                                                 \
    } while (false)

#define CHECK_THROWS_AS(expression, ExceptionType)                        \
    do {                                                                  \
        bool caught_ = false;                                             \
        try {                                                             \
            (void)(expression);                                           \
        } catch (const ExceptionType&) {                                  \
            caught_ = true;                                               \
        } catch (...) {                                                   \
        }                                                                 \
        if (!caught_) {                                                   \
            ::pstest::fail(__FILE__, __LINE__,                            \
                           "expected " #ExceptionType " from " #expression); \
        }                                                                 \
    } while (false)
