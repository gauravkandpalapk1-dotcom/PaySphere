#pragma once

#include "database/IdempotencyRepository.h"
#include "database/TransactionRepository.h"
#include "models/Transaction.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {
class DatabaseManager;
}

namespace paysphere::services {

// Duplicate-request protection shared by deposits and payments.
//
// A client sends a unique key with each money request. The first request
// with a key is processed and its transaction remembered under the key; a
// repeat with the same key and the same request returns that transaction
// instead of moving money again. Reusing a key for a *different* request is
// refused. Keys are scoped to a user and never expire.
class IdempotencyLedger {
public:
    explicit IdempotencyLedger(db::DatabaseManager& database);

    // 8-128 letters, digits, '-' or '_'. Throws ValidationError.
    static void validateKey(const std::string& key);
    // A fresh random key (32 hex characters).
    static std::string newKey();
    // A fixed-length digest of the canonical request text (never include secrets).
    static std::string fingerprint(const std::string& canonicalRequest);

    // The transaction produced by an earlier identical request, if any.
    // Throws IdempotencyConflictError if the key was used for a different request.
    std::optional<models::Transaction> replay(std::int64_t userId, const std::string& key,
                                              const std::string& fingerprint);
    // Call inside the transaction that recorded `transactionId`.
    void remember(std::int64_t userId, const std::string& key, const std::string& fingerprint,
                  std::int64_t transactionId, const std::string& createdAt);

private:
    db::IdempotencyRepository keys_;
    db::TransactionRepository transactions_;
};

} // namespace paysphere::services
