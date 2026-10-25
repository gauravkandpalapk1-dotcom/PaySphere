#pragma once

#include <chrono>
#include <string>

namespace paysphere::services {

using TimePoint = std::chrono::system_clock::time_point;

// Source of the current time. Services depend on this abstraction instead of
// calling system_clock directly, so tests can move time forward (lockout
// expiry, session timeouts) without sleeping.
class Clock {
public:
    virtual ~Clock();
    virtual TimePoint now() const = 0;
};

class SystemClock final : public Clock {
public:
    TimePoint now() const override;
};

// UTC timestamps in the database's format, e.g. "2026-10-14T05:30:00Z".
// Seconds precision: sub-second parts are truncated.
std::string formatTimestamp(TimePoint time);
// Strict inverse of formatTimestamp; throws ValidationError otherwise.
TimePoint parseTimestamp(const std::string& text);

// For people: Indian Standard Time (UTC+05:30, no daylight saving),
// e.g. "14 Oct 2026, 03:30:00 PM IST".
std::string formatIndianTime(TimePoint time);

} // namespace paysphere::services
