#include "payments/Money.h"

#include "models/Errors.h"

#include <algorithm>
#include <limits>
#include <ostream>

namespace paysphere::payments {

namespace {

// 15 rupee digits is at most ~10^17 paise, well inside int64_t, so parsing
// can never overflow.
constexpr std::size_t kMaxRupeeDigits = 15;

bool allDigits(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

ValidationError invalidAmount() {
    return ValidationError("Invalid amount: enter rupees with at most two decimal places, e.g. 250 or 250.50");
}

} // namespace

Money Money::parse(const std::string& rupees) {
    const std::size_t dot = rupees.find('.');
    const std::string whole = rupees.substr(0, dot);
    const std::string fraction = dot == std::string::npos ? std::string() : rupees.substr(dot + 1);

    if (whole.empty() || whole.size() > kMaxRupeeDigits || !allDigits(whole)) {
        throw invalidAmount();
    }
    if (dot != std::string::npos && (fraction.empty() || fraction.size() > 2 || !allDigits(fraction))) {
        throw invalidAmount();
    }

    Paise paise = 0;
    for (char digit : whole) {
        paise = paise * 10 + (digit - '0');
    }
    paise *= 100;
    if (!fraction.empty()) {
        paise += (fraction[0] - '0') * 10;
    }
    if (fraction.size() == 2) {
        paise += fraction[1] - '0';
    }
    return Money(paise);
}

Money Money::operator+(Money other) const {
    constexpr Paise max = std::numeric_limits<Paise>::max();
    constexpr Paise min = std::numeric_limits<Paise>::min();
    if ((other.paise_ > 0 && paise_ > max - other.paise_) ||
        (other.paise_ < 0 && paise_ < min - other.paise_)) {
        throw ValidationError("Amount is out of range");
    }
    return Money(paise_ + other.paise_);
}

Money Money::operator-(Money other) const {
    constexpr Paise max = std::numeric_limits<Paise>::max();
    constexpr Paise min = std::numeric_limits<Paise>::min();
    if ((other.paise_ > 0 && paise_ < min + other.paise_) ||
        (other.paise_ < 0 && paise_ > max + other.paise_)) {
        throw ValidationError("Amount is out of range");
    }
    return Money(paise_ - other.paise_);
}

std::string Money::toString() const {
    // Unsigned arithmetic keeps the magnitude of INT64_MIN representable.
    const bool negative = paise_ < 0;
    const auto raw = static_cast<std::uint64_t>(paise_);
    const std::uint64_t magnitude = negative ? 0 - raw : raw;

    const std::string rupees = std::to_string(magnitude / 100);
    const auto remainder = static_cast<unsigned>(magnitude % 100);

    // Indian grouping: last three digits, then groups of two (12,34,567).
    std::string grouped;
    const std::size_t length = rupees.size();
    for (std::size_t i = 0; i < length; ++i) {
        grouped += rupees[i];
        const std::size_t digitsAfter = length - i - 1;
        if (digitsAfter >= 3 && digitsAfter % 2 == 1) {
            grouped += ',';
        }
    }

    std::string result = negative ? "-Rs. " : "Rs. ";
    result += grouped;
    result += '.';
    result += static_cast<char>('0' + remainder / 10);
    result += static_cast<char>('0' + remainder % 10);
    return result;
}

std::ostream& operator<<(std::ostream& out, Money money) {
    return out << money.toString();
}

} // namespace paysphere::payments
