#pragma once

#include <cstddef>
#include <string>

namespace paysphere::services {

// Abstraction over password hashing, so the authentication code does not
// depend on a particular algorithm or library.
//
// A hash is a self-describing string that embeds the algorithm, its cost
// parameters and a random salt, so verify() needs nothing else and
// parameters can be raised later without breaking existing accounts.
class PasswordHasher {
public:
    virtual ~PasswordHasher();

    virtual std::string hash(const std::string& password) const = 0;
    // Constant-time comparison; false (not an exception) for a wrong password
    // or a malformed hash.
    virtual bool verify(const std::string& encodedHash, const std::string& password) const = 0;
    // True if encodedHash used weaker parameters than this hasher's current ones.
    virtual bool needsRehash(const std::string& encodedHash) const = 0;
};

// Argon2id via libsodium's crypto_pwhash_str. Argon2id won the Password
// Hashing Competition and is OWASP's first recommendation. It is
// "memory-hard": each guess needs a large block of RAM, which makes GPU and
// ASIC cracking expensive.
class SodiumPasswordHasher final : public PasswordHasher {
public:
    struct Cost {
        unsigned long long opsLimit; // passes over memory
        std::size_t memLimitBytes;

        // libsodium's INTERACTIVE preset: 2 passes, 64 MiB, roughly 0.1-0.3 s
        // per hash on a laptop. Suitable for logins.
        static Cost interactive();
        // The smallest parameters libsodium accepts. Tests only: it keeps
        // the suite fast but offers no real protection.
        static Cost minimumForTests();
    };

    SodiumPasswordHasher();
    explicit SodiumPasswordHasher(Cost cost);

    std::string hash(const std::string& password) const override;
    bool verify(const std::string& encodedHash, const std::string& password) const override;
    bool needsRehash(const std::string& encodedHash) const override;

private:
    Cost cost_;
};

} // namespace paysphere::services
