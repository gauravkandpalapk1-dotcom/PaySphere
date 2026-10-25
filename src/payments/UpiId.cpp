#include "payments/UpiId.h"

#include "models/Errors.h"

#include <algorithm>

namespace paysphere::payments {

namespace {

bool isLowerAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

std::string normalize(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::string text = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

} // namespace

UpiId::UpiId(const std::string& input) : value_(normalize(input)) {
    const std::size_t at = value_.find('@');
    const std::string handle = at == std::string::npos ? std::string() : value_.substr(0, at);
    const std::string provider = at == std::string::npos ? std::string() : value_.substr(at + 1);

    const bool handleOk = handle.size() >= 3 && handle.size() <= 50 && isLowerAlnum(handle.front()) &&
                          std::all_of(handle.begin(), handle.end(), [](char c) {
                              return isLowerAlnum(c) || c == '.' || c == '_' || c == '-';
                          });
    const bool providerOk = provider.size() >= 2 && provider.size() <= 20 &&
                            std::all_of(provider.begin(), provider.end(),
                                        [](char c) { return c >= 'a' && c <= 'z'; });
    if (!handleOk || !providerOk) {
        throw ValidationError("Enter a UPI ID in the form name@bank, e.g. asha@paysphere");
    }
}

} // namespace paysphere::payments
