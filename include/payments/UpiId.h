#pragma once

#include <string>

namespace paysphere::payments {

// A validated, normalized UPI ID (value object): "handle@provider".
//   - handle: 3-50 characters of letters, digits, '.', '_' or '-',
//     starting with a letter or digit;
//   - provider: 2-20 letters.
// Stored lowercase, because UPI IDs are case-insensitive. An invalid UPI ID
// cannot be constructed, so code holding a UpiId never has to re-check it.
class UpiId {
public:
    // Trims, lowercases and validates; throws ValidationError.
    explicit UpiId(const std::string& input);

    const std::string& value() const noexcept { return value_; }
    // The part before '@', e.g. "asha" in "asha@paysphere".
    std::string handle() const { return value_.substr(0, value_.find('@')); }

    friend bool operator==(const UpiId& a, const UpiId& b) noexcept { return a.value_ == b.value_; }
    friend bool operator!=(const UpiId& a, const UpiId& b) noexcept { return a.value_ != b.value_; }

private:
    std::string value_;
};

} // namespace paysphere::payments
