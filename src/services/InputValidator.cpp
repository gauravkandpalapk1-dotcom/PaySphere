#include "services/InputValidator.h"

#include "models/Errors.h"
#include "payments/UpiId.h"

#include <algorithm>
#include <array>

namespace paysphere::services {

namespace {

bool isAsciiLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isLowerAlnum(char c) {
    return (c >= 'a' && c <= 'z') || isAsciiDigit(c);
}

bool isControl(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7f;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

bool isValidDomain(const std::string& domain) {
    if (domain.empty() || domain.size() > 253) {
        return false;
    }
    std::size_t labels = 0;
    std::size_t start = 0;
    std::string lastLabel;
    while (start <= domain.size()) {
        const std::size_t dot = std::min(domain.find('.', start), domain.size());
        const std::string label = domain.substr(start, dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), [](char c) { return isLowerAlnum(c) || c == '-'; })) {
            return false;
        }
        ++labels;
        lastLabel = label;
        start = dot + 1;
    }
    // A top-level domain of letters only, e.g. ".com" or ".in".
    return labels >= 2 && lastLabel.size() >= 2 &&
           std::all_of(lastLabel.begin(), lastLabel.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

// A short sample of the most common leaked passwords that still meet the
// length rule. A production system would check a large breach corpus.
constexpr std::array<const char*, 20> kCommonPasswords = {
    "password", "password1", "password123", "12345678", "123456789", "1234567890",
    "11111111", "00000000", "qwerty123", "qwertyuiop", "iloveyou", "abc12345",
    "admin123", "welcome1", "welcome123", "letmein1", "sunshine", "princess",
    "paysphere", "paysphere123"};

} // namespace

std::string InputValidator::fullName(const std::string& input) {
    const std::string name = trim(input);
    if (name.size() < 2 || name.size() > 100) {
        throw ValidationError("Full name must be 2 to 100 characters long");
    }
    if (std::any_of(name.begin(), name.end(), isControl)) {
        throw ValidationError("Full name contains invalid characters");
    }
    if (!std::any_of(name.begin(), name.end(), isAsciiLetter)) {
        throw ValidationError("Full name must contain letters");
    }
    return name;
}

std::string InputValidator::email(const std::string& input) {
    const std::string email = toLower(trim(input));
    const std::size_t at = email.find('@');
    if (email.size() > kMaxIdentifierLength || at == std::string::npos ||
        email.find('@', at + 1) != std::string::npos) {
        throw ValidationError("Enter a valid email address, e.g. name@example.com");
    }
    const std::string local = email.substr(0, at);
    const std::string domain = email.substr(at + 1);
    const bool localOk =
        !local.empty() && local.size() <= 64 && local.front() != '.' && local.back() != '.' &&
        local.find("..") == std::string::npos &&
        std::all_of(local.begin(), local.end(), [](char c) {
            return isLowerAlnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
        });
    if (!localOk || !isValidDomain(domain)) {
        throw ValidationError("Enter a valid email address, e.g. name@example.com");
    }
    return email;
}

std::string InputValidator::phone(const std::string& input) {
    std::string digits;
    for (char c : trim(input)) {
        if (c == ' ' || c == '-') {
            continue;
        }
        digits += c;
    }
    if (digits.rfind("+91", 0) == 0) {
        digits.erase(0, 3);
    }
    if (digits.size() != 10 || !std::all_of(digits.begin(), digits.end(), isAsciiDigit) ||
        digits.front() < '6') {
        throw ValidationError("Enter a 10-digit Indian mobile number starting with 6, 7, 8 or 9");
    }
    return digits;
}

std::string InputValidator::upiId(const std::string& input) {
    return payments::UpiId(input).value(); // single source of truth for UPI ID rules
}

void InputValidator::password(const std::string& password,
                              const std::vector<std::string>& personalDetails) {
    if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength) {
        throw ValidationError("Password must be 8 to 128 characters long");
    }
    if (trim(password).empty()) {
        throw ValidationError("Password must not be only spaces");
    }
    const std::string lowered = toLower(password);
    for (const char* common : kCommonPasswords) {
        if (lowered == common) {
            throw ValidationError("This password is too common; choose another");
        }
    }
    for (const std::string& detail : personalDetails) {
        if (detail.size() >= 4 && lowered.find(toLower(detail)) != std::string::npos) {
            throw ValidationError("Password must not contain your name, email, UPI ID or phone number");
        }
    }
}

std::string InputValidator::loginIdentifier(const std::string& input) {
    const std::string identifier = toLower(trim(input));
    if (identifier.empty() || identifier.size() > kMaxIdentifierLength) {
        throw ValidationError("Enter your email or UPI ID");
    }
    return identifier;
}

} // namespace paysphere::services
