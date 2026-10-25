#include "TestFramework.h"

#include "models/Errors.h"
#include "payments/Money.h"

#include <cstdint>
#include <limits>

using paysphere::ValidationError;
using paysphere::payments::Money;

TEST(money_parses_whole_and_fractional_rupees) {
    CHECK_EQ(Money::parse("0").paise(), 0);
    CHECK_EQ(Money::parse("250").paise(), 25000);
    CHECK_EQ(Money::parse("250.5").paise(), 25050);
    CHECK_EQ(Money::parse("250.05").paise(), 25005);
    CHECK_EQ(Money::parse("0.01").paise(), 1);
    CHECK_EQ(Money::parse("999999999999999.99").paise(), INT64_C(99999999999999999));
}

TEST(money_rejects_malformed_amounts) {
    const char* invalid[] = {"", "-5", "+5", "1.234", "1.", ".5", "1,000", " 1", "1 ",
                             "abc", "1.2.3", "1e3", "1000000000000000"};
    for (const char* text : invalid) {
        CHECK_THROWS_AS(Money::parse(text), ValidationError);
    }
}

TEST(money_formats_with_indian_digit_grouping) {
    CHECK_EQ(Money::zero().toString(), "Rs. 0.00");
    CHECK_EQ(Money::fromPaise(5).toString(), "Rs. 0.05");
    CHECK_EQ(Money::fromPaise(99999).toString(), "Rs. 999.99");
    CHECK_EQ(Money::fromPaise(123456).toString(), "Rs. 1,234.56");
    CHECK_EQ(Money::fromPaise(1234567).toString(), "Rs. 12,345.67");
    CHECK_EQ(Money::fromPaise(123456789).toString(), "Rs. 12,34,567.89");
    CHECK_EQ(Money::fromPaise(-150).toString(), "-Rs. 1.50");
}

TEST(money_formats_extreme_values_without_overflow) {
    CHECK_EQ(Money::fromPaise(std::numeric_limits<std::int64_t>::min()).toString(),
             "-Rs. 92,23,37,20,36,85,47,758.08");
}

TEST(money_arithmetic_and_comparison) {
    const Money a = Money::parse("100.50");
    const Money b = Money::parse("0.75");
    CHECK_EQ((a + b).paise(), 10125);
    CHECK_EQ((a - b).paise(), 9975);
    CHECK((b - a).isNegative());
    CHECK(a > b);
    CHECK(b < a);
    CHECK(a == Money::fromPaise(10050));
    CHECK(a != b);
}

TEST(money_arithmetic_detects_overflow) {
    const Money max = Money::fromPaise(std::numeric_limits<std::int64_t>::max());
    const Money min = Money::fromPaise(std::numeric_limits<std::int64_t>::min());
    CHECK_THROWS_AS(max + Money::fromPaise(1), ValidationError);
    CHECK_THROWS_AS(min - Money::fromPaise(1), ValidationError);
    CHECK_THROWS_AS(Money::zero() - min, ValidationError);
}
