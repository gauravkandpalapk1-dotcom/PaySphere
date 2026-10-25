#include "services/PasswordHasher.h"

#include "Sodium.h"
#include "models/Errors.h"

#include <sodium.h>

namespace paysphere::services {

PasswordHasher::~PasswordHasher() = default;

SodiumPasswordHasher::Cost SodiumPasswordHasher::Cost::interactive() {
    return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE};
}

SodiumPasswordHasher::Cost SodiumPasswordHasher::Cost::minimumForTests() {
    return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN};
}

SodiumPasswordHasher::SodiumPasswordHasher() : SodiumPasswordHasher(Cost::interactive()) {}

SodiumPasswordHasher::SodiumPasswordHasher(Cost cost) : cost_(cost) {
    detail::ensureSodiumInitialized();
    if (cost_.opsLimit < crypto_pwhash_OPSLIMIT_MIN || cost_.memLimitBytes < crypto_pwhash_MEMLIMIT_MIN) {
        throw ValidationError("Password hashing cost is below libsodium's minimum");
    }
}

std::string SodiumPasswordHasher::hash(const std::string& password) const {
    char encoded[crypto_pwhash_STRBYTES];
    // crypto_pwhash_str generates a random salt itself and returns
    // "$argon2id$v=19$m=<KiB>,t=<passes>,p=1$<salt>$<hash>".
    if (crypto_pwhash_str(encoded, password.data(), password.size(), cost_.opsLimit,
                          cost_.memLimitBytes) != 0) {
        throw PaySphereError("Password hashing failed (out of memory)");
    }
    return encoded;
}

bool SodiumPasswordHasher::verify(const std::string& encodedHash, const std::string& password) const {
    // The function reads a NUL-terminated string of at most STRBYTES.
    if (encodedHash.empty() || encodedHash.size() >= crypto_pwhash_STRBYTES) {
        return false;
    }
    return crypto_pwhash_str_verify(encodedHash.c_str(), password.data(), password.size()) == 0;
}

bool SodiumPasswordHasher::needsRehash(const std::string& encodedHash) const {
    if (encodedHash.empty() || encodedHash.size() >= crypto_pwhash_STRBYTES) {
        return true;
    }
    // 0 = current, 1 = weaker parameters, -1 = not an Argon2id hash.
    return crypto_pwhash_str_needs_rehash(encodedHash.c_str(), cost_.opsLimit, cost_.memLimitBytes) != 0;
}

} // namespace paysphere::services
