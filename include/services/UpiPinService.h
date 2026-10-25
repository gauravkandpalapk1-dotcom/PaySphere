#pragma once

#include "database/UpiPinRepository.h"
#include "payments/PaymentMethod.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::services {

class AuthService;
class Clock;
class PasswordHasher;

// After maxFailures wrong PINs within `window` (counted since the last
// correct PIN), payments are blocked for blockDuration. Real UPI apps block
// after 3 wrong PINs; the exact durations here are a simulation choice.
struct PinPolicy {
    int maxFailures = 3;
    std::chrono::seconds window{std::chrono::hours(24)};
    std::chrono::seconds blockDuration{std::chrono::hours(24)};
};

// The demo UPI PIN: setting it, checking it and rate-limiting wrong guesses.
//
// A PIN has only 10^4 or 10^6 possible values, so its hash on its own would
// not survive an offline brute-force attack; the online attempt limit is the
// main protection. A real system would also add a secret key ("pepper") kept
// in a hardware security module, outside the database.
class UpiPinService final : public payments::CredentialVerifier {
public:
    UpiPinService(db::DatabaseManager& database, AuthService& auth, const PasswordHasher& hasher,
                  const Clock& clock, PinPolicy policy = {});

    // Sets the caller's PIN once. Rejects bad formats and trivially guessable
    // PINs (one repeated digit, or a straight sequence such as 1234).
    // Throws SessionError or ValidationError.
    void setPin(const std::string& sessionToken, const std::string& pin);
    bool hasPin(std::int64_t userId);

    // Checks a PIN against the stored hash. It does not record the attempt;
    // the caller records it with recordAttempt() inside its transaction.
    bool verifyCredential(std::int64_t userId, const std::string& pin) override;
    void recordAttempt(std::int64_t userId, bool succeeded);

    // When payments are blocked by wrong PINs, the time the block ends.
    std::optional<std::string> blockedUntil(std::int64_t userId);

    // Throws ValidationError for formats or trivially guessable PINs.
    static void validateNewPin(const std::string& pin);

private:
    AuthService& auth_;
    const PasswordHasher& hasher_;
    const Clock& clock_;
    PinPolicy policy_;
    db::UpiPinRepository pins_;
};

} // namespace paysphere::services
