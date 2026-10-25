#include "services/Clock.h"

#include "models/Errors.h"

#include <cstdio>

namespace paysphere::services {

namespace {

constexpr long long kSecondsPerDay = 86400;

// Calendar conversions from Howard Hinnant's public-domain date algorithms
// (http://howardhinnant.github.io/date_algorithms.html). They avoid gmtime and
// timegm, which are not thread-safe or not portable.
long long daysFromCivil(long long year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<long long>(dayOfEra) - 719468;
}

struct CivilDate {
    long long year;
    unsigned month;
    unsigned day;
};

CivilDate civilFromDays(long long days) {
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const unsigned month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const long long year = static_cast<long long>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
    return {year, month, day};
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

unsigned readNumber(const std::string& text, std::size_t position, std::size_t digits) {
    unsigned value = 0;
    for (std::size_t i = position; i < position + digits; ++i) {
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    return value;
}

} // namespace

Clock::~Clock() = default;

TimePoint SystemClock::now() const {
    return std::chrono::system_clock::now();
}

std::string formatTimestamp(TimePoint time) {
    const long long seconds =
        std::chrono::floor<std::chrono::seconds>(time).time_since_epoch().count();
    long long days = seconds / kSecondsPerDay;
    long long secondOfDay = seconds % kSecondsPerDay;
    if (secondOfDay < 0) {
        secondOfDay += kSecondsPerDay;
        --days;
    }
    const CivilDate date = civilFromDays(days);

    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", date.year,
                  date.month, date.day, secondOfDay / 3600, secondOfDay / 60 % 60, secondOfDay % 60);
    return buffer;
}

std::string formatIndianTime(TimePoint time) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    constexpr long long kIstOffsetSeconds = 5 * 3600 + 30 * 60;

    const long long seconds =
        std::chrono::floor<std::chrono::seconds>(time).time_since_epoch().count() + kIstOffsetSeconds;
    long long days = seconds / kSecondsPerDay;
    long long secondOfDay = seconds % kSecondsPerDay;
    if (secondOfDay < 0) {
        secondOfDay += kSecondsPerDay;
        --days;
    }
    const CivilDate date = civilFromDays(days);
    const long long hour = secondOfDay / 3600;
    const long long hour12 = hour % 12 == 0 ? 12 : hour % 12;

    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%02u %s %04lld, %02lld:%02lld:%02lld %s IST", date.day,
                  kMonths[date.month - 1], date.year, hour12, secondOfDay / 60 % 60, secondOfDay % 60,
                  hour < 12 ? "AM" : "PM");
    return buffer;
}

TimePoint parseTimestamp(const std::string& text) {
    // Exactly "YYYY-MM-DDTHH:MM:SSZ".
    static const char kPattern[] = "dddd-dd-ddTdd:dd:ddZ";
    bool wellFormed = text.size() == sizeof kPattern - 1;
    for (std::size_t i = 0; wellFormed && i < text.size(); ++i) {
        wellFormed = kPattern[i] == 'd' ? isDigit(text[i]) : text[i] == kPattern[i];
    }
    if (!wellFormed) {
        throw ValidationError("Invalid timestamp: " + text);
    }

    const unsigned month = readNumber(text, 5, 2);
    const unsigned day = readNumber(text, 8, 2);
    const long long days = daysFromCivil(readNumber(text, 0, 4), month, day);
    const long long seconds = days * kSecondsPerDay + readNumber(text, 11, 2) * 3600LL +
                              readNumber(text, 14, 2) * 60LL + readNumber(text, 17, 2);
    const TimePoint result{std::chrono::seconds(seconds)};

    // Round-tripping rejects out-of-range fields such as month 13, Feb 30 or hour 24.
    if (month < 1 || month > 12 || day < 1 || formatTimestamp(result) != text) {
        throw ValidationError("Invalid timestamp: " + text);
    }
    return result;
}

} // namespace paysphere::services
