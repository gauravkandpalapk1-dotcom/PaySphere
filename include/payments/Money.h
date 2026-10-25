#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace paysphere::payments {

// An amount of Indian rupees stored as a whole number of paise
// (1 rupee = 100 paise). Integer storage avoids floating-point rounding
// errors, and arithmetic is checked for overflow.
class Money {
public:
    using Paise = std::int64_t;

    constexpr Money() noexcept = default;

    static constexpr Money fromPaise(Paise paise) noexcept { return Money(paise); }
    static constexpr Money zero() noexcept { return Money(); }

    // Parses a non-negative rupee amount such as "250", "250.5" or "250.75".
    // Rejects signs, whitespace, digit separators and more than two decimal
    // places. Throws ValidationError.
    static Money parse(const std::string& rupees);

    constexpr Paise paise() const noexcept { return paise_; }
    constexpr bool isZero() const noexcept { return paise_ == 0; }
    constexpr bool isPositive() const noexcept { return paise_ > 0; }
    constexpr bool isNegative() const noexcept { return paise_ < 0; }

    // Throw ValidationError if the result would overflow.
    Money operator+(Money other) const;
    Money operator-(Money other) const;

    // Indian digit grouping, e.g. "Rs. 12,34,567.89".
    std::string toString() const;

    friend constexpr bool operator==(Money a, Money b) noexcept { return a.paise_ == b.paise_; }
    friend constexpr bool operator!=(Money a, Money b) noexcept { return a.paise_ != b.paise_; }
    friend constexpr bool operator<(Money a, Money b) noexcept { return a.paise_ < b.paise_; }
    friend constexpr bool operator<=(Money a, Money b) noexcept { return a.paise_ <= b.paise_; }
    friend constexpr bool operator>(Money a, Money b) noexcept { return a.paise_ > b.paise_; }
    friend constexpr bool operator>=(Money a, Money b) noexcept { return a.paise_ >= b.paise_; }

private:
    explicit constexpr Money(Paise paise) noexcept : paise_(paise) {}

    Paise paise_ = 0;
};

std::ostream& operator<<(std::ostream& out, Money money);

} // namespace paysphere::payments
