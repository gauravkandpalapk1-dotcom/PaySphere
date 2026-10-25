#include "Sodium.h"

#include "models/Errors.h"

#include <sodium.h>

#include <array>
#include <mutex>
#include <vector>

namespace paysphere::services::detail {

void ensureSodiumInitialized() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] { ready = sodium_init() >= 0; }); // 1 means "already initialized"
    if (!ready) {
        throw PaySphereError("The cryptography library (libsodium) failed to initialize");
    }
}

std::string toHex(const unsigned char* bytes, std::size_t length) {
    std::string hex(length * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), bytes, length);
    hex.pop_back(); // drop the terminating NUL written by sodium_bin2hex
    return hex;
}

std::string blake2bHex(const std::string& text) {
    ensureSodiumInitialized();
    std::array<unsigned char, crypto_generichash_BYTES> digest{};
    crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(text.data()),
                       text.size(), nullptr, 0);
    return toHex(digest.data(), digest.size());
}

std::string randomHex(std::size_t byteCount) {
    ensureSodiumInitialized();
    std::vector<unsigned char> random(byteCount);
    randombytes_buf(random.data(), random.size());
    std::string hex = toHex(random.data(), random.size());
    sodium_memzero(random.data(), random.size());
    return hex;
}

} // namespace paysphere::services::detail
