#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

// Recent failed attempts counted towards a lockout (logins or UPI PINs).
struct FailureSummary {
    std::int64_t failures = 0;
    std::optional<std::string> lastFailureAt; // UTC timestamp of the newest failure
};

} // namespace paysphere::db
