#include "payments/SecretString.h"

#include <sodium.h>

#include <utility>

namespace paysphere::payments {

void secureWipe(std::string& text) noexcept {
    // A moved-from or shortened string can still hold old characters in its
    // buffer, so wipe the whole capacity, not just size().
    text.resize(text.capacity());
    sodium_memzero(text.data(), text.size());
    text.clear();
}

SecretString::SecretString(std::string&& value) : value_(std::move(value)) {
    secureWipe(value);
}

SecretString::~SecretString() {
    secureWipe(value_);
}

} // namespace paysphere::payments
