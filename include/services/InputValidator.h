#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace paysphere::services {

// Validates and normalizes user input. Each function returns the normalized
// value (trimmed and, where matching is case-insensitive, lowercased) or
// throws ValidationError with a message suitable for showing to the user.
class InputValidator {
public:
    static constexpr std::size_t kMinPasswordLength = 8;
    static constexpr std::size_t kMaxPasswordLength = 128;
    // Matches login_attempts.identifier, which stores what was typed at login.
    static constexpr std::size_t kMaxIdentifierLength = 100;

    // 2-100 characters, at least one letter, no control characters.
    static std::string fullName(const std::string& input);
    // A practical subset of RFC 5321: local@domain.tld in ASCII, at most 100
    // characters; lowercased.
    static std::string email(const std::string& input);
    // Indian mobile number: 10 digits starting with 6-9. Spaces, hyphens and
    // a "+91" prefix are accepted and removed.
    static std::string phone(const std::string& input);
    // handle@provider: a 3-50 character handle of letters, digits, '.', '_'
    // or '-' starting with a letter or digit; a 2-20 letter provider.
    // Lowercased.
    static std::string upiId(const std::string& input);
    // NIST SP 800-63B style: 8-128 characters, not a commonly used password,
    // and not containing personal details (e.g. the email name or phone).
    // No composition rules such as "one symbol", which NIST advises against.
    static void password(const std::string& password, const std::vector<std::string>& personalDetails);
    // An email or UPI ID typed at login: trimmed and lowercased.
    static std::string loginIdentifier(const std::string& input);
};

} // namespace paysphere::services
