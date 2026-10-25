#pragma once

#include <chrono>
#include <cstdint>

namespace paysphere::models {

// A snapshot of an authenticated session. It identifies the user and the
// session's lifetime; the secret token itself is never stored here.
class Session {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    Session(std::int64_t userId, TimePoint createdAt, TimePoint lastActivityAt, TimePoint expiresAt);

    std::int64_t userId() const noexcept { return userId_; }
    TimePoint createdAt() const noexcept { return createdAt_; }
    TimePoint lastActivityAt() const noexcept { return lastActivityAt_; }
    // The earlier of the idle deadline and the absolute deadline.
    TimePoint expiresAt() const noexcept { return expiresAt_; }

private:
    std::int64_t userId_;
    TimePoint createdAt_;
    TimePoint lastActivityAt_;
    TimePoint expiresAt_;
};

} // namespace paysphere::models
