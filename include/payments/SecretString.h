#pragma once

#include <string>

namespace paysphere::payments {

// Overwrites every byte a string has allocated (including the unused part of
// its buffer) in a way the compiler cannot optimize away, then empties it.
void secureWipe(std::string& text) noexcept;

// Holds a secret such as a UPI PIN and wipes it from memory when destroyed,
// including when an enclosing constructor throws (members are always
// destroyed). Not copyable, so the secret exists in exactly one place.
class SecretString {
public:
    // Takes over the text and wipes what is left in the source string.
    explicit SecretString(std::string&& value);
    ~SecretString();

    SecretString(const SecretString&) = delete;
    SecretString& operator=(const SecretString&) = delete;

    const std::string& value() const noexcept { return value_; }

private:
    std::string value_;
};

} // namespace paysphere::payments
