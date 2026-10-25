#include "services/ReferenceGenerator.h"

#include "Sodium.h"

#include <sodium.h>

#include <array>

namespace paysphere::services {

ReferenceGenerator::~ReferenceGenerator() = default;

RandomReferenceGenerator::RandomReferenceGenerator() {
    detail::ensureSodiumInitialized();
}

std::string RandomReferenceGenerator::next(models::TransactionType type, TimePoint now) {
    static const char kHexDigits[] = "0123456789ABCDEF";

    std::string reference = type == models::TransactionType::Deposit ? "DEP" : "TRF";
    const std::string timestamp = formatTimestamp(now); // "2026-10-14T..."
    reference += timestamp.substr(0, 4) + timestamp.substr(5, 2) + timestamp.substr(8, 2);

    std::array<unsigned char, 6> random{};
    randombytes_buf(random.data(), random.size());
    for (unsigned char byte : random) {
        reference += kHexDigits[byte >> 4];
        reference += kHexDigits[byte & 0x0f];
    }
    return reference;
}

} // namespace paysphere::services
