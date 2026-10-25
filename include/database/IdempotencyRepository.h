#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

class DatabaseManager;

struct IdempotencyRecord {
    std::string requestFingerprint;
    std::int64_t transactionId;
};

// Persistence for idempotency_keys: which transaction a user's request key
// produced. The UNIQUE (user_id, idempotency_key) constraint guarantees that
// one key can never produce two transactions.
class IdempotencyRepository {
public:
    explicit IdempotencyRepository(DatabaseManager& database);

    std::optional<IdempotencyRecord> find(std::int64_t userId, const std::string& key);
    // Throws ConstraintViolation if the key is already used by this user.
    void insert(std::int64_t userId, const std::string& key, const std::string& requestFingerprint,
                std::int64_t transactionId, const std::string& createdAt);

private:
    DatabaseManager& database_;
};

} // namespace paysphere::db
