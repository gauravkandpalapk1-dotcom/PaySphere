#include "services/UpiPinService.h"

#include "database/DatabaseError.h"
#include "models/Errors.h"
#include "payments/UPIPayment.h"
#include "services/AuthService.h"
#include "services/Clock.h"
#include "services/PasswordHasher.h"

namespace paysphere::services {

UpiPinService::UpiPinService(db::DatabaseManager& database, AuthService& auth,
                             const PasswordHasher& hasher, const Clock& clock, PinPolicy policy)
    : auth_(auth), hasher_(hasher), clock_(clock), policy_(policy), pins_(database) {
    if (policy_.maxFailures < 1 || policy_.window.count() <= 0 || policy_.blockDuration.count() <= 0) {
        throw ValidationError("PIN policy values must be positive");
    }
}

void UpiPinService::validateNewPin(const std::string& pin) {
    payments::UPIPayment::validatePinFormat(pin);

    bool allSame = true;
    bool ascending = true;
    bool descending = true;
    for (std::size_t i = 1; i < pin.size(); ++i) {
        allSame = allSame && pin[i] == pin[0];
        ascending = ascending && pin[i] == pin[i - 1] + 1;
        descending = descending && pin[i] == pin[i - 1] - 1;
    }
    if (allSame || ascending || descending) {
        throw ValidationError("This UPI PIN is too easy to guess; avoid repeated digits and sequences");
    }
}

void UpiPinService::setPin(const std::string& sessionToken, const std::string& pin) {
    const models::User user = auth_.authenticate(sessionToken);
    validateNewPin(pin);
    if (hasPin(user.id())) {
        throw ValidationError("A UPI PIN is already set for this account");
    }
    try {
        pins_.insert(user.id(), hasher_.hash(pin));
    } catch (const db::ConstraintViolation& error) {
        if (error.isUniqueViolation()) { // set concurrently by another request
            throw ValidationError("A UPI PIN is already set for this account");
        }
        throw;
    }
}

bool UpiPinService::hasPin(std::int64_t userId) {
    return pins_.findHash(userId).has_value();
}

bool UpiPinService::verifyCredential(std::int64_t userId, const std::string& pin) {
    const std::optional<std::string> hash = pins_.findHash(userId);
    return hash.has_value() && hasher_.verify(*hash, pin);
}

void UpiPinService::recordAttempt(std::int64_t userId, bool succeeded) {
    pins_.recordAttempt(userId, succeeded, formatTimestamp(clock_.now()));
}

std::optional<std::string> UpiPinService::blockedUntil(std::int64_t userId) {
    const TimePoint now = clock_.now();
    const db::FailureSummary failures = pins_.recentFailures(userId, formatTimestamp(now - policy_.window));
    if (failures.failures < policy_.maxFailures || !failures.lastFailureAt) {
        return std::nullopt;
    }
    const TimePoint until = parseTimestamp(*failures.lastFailureAt) + policy_.blockDuration;
    if (now >= until) {
        return std::nullopt;
    }
    return formatTimestamp(until);
}

} // namespace paysphere::services
