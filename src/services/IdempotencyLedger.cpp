#include "services/IdempotencyLedger.h"

#include "Sodium.h"
#include "database/DatabaseError.h"
#include "models/Errors.h"

#include <algorithm>

namespace paysphere::services {

IdempotencyLedger::IdempotencyLedger(db::DatabaseManager& database) : keys_(database), transactions_(database) {}

void IdempotencyLedger::validateKey(const std::string& key) {
    const bool allowed = std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_';
    });
    if (key.size() < 8 || key.size() > 128 || !allowed) {
        throw ValidationError("Idempotency key must be 8-128 letters, digits, '-' or '_'");
    }
}

std::string IdempotencyLedger::newKey() {
    return detail::randomHex(16);
}

std::string IdempotencyLedger::fingerprint(const std::string& canonicalRequest) {
    // Fixed length whatever the input, and no request details stored in clear.
    return detail::blake2bHex(canonicalRequest);
}

std::optional<models::Transaction> IdempotencyLedger::replay(std::int64_t userId, const std::string& key,
                                                             const std::string& fingerprint) {
    const std::optional<db::IdempotencyRecord> record = keys_.find(userId, key);
    if (!record) {
        return std::nullopt;
    }
    if (record->requestFingerprint != fingerprint) {
        throw IdempotencyConflictError("This idempotency key was already used for a different request; use a new key");
    }
    std::optional<models::Transaction> original = transactions_.findById(record->transactionId);
    if (!original) {
        // The schema forbids this (foreign key + append-only transactions).
        throw db::CorruptDataError("Idempotency key refers to a missing transaction");
    }
    return original;
}

void IdempotencyLedger::remember(std::int64_t userId, const std::string& key, const std::string& fingerprint,
                                 std::int64_t transactionId, const std::string& createdAt) {
    keys_.insert(userId, key, fingerprint, transactionId, createdAt);
}

} // namespace paysphere::services
