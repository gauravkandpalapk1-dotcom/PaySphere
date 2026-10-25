#include "database/IdempotencyRepository.h"

#include "database/DatabaseManager.h"
#include "database/Statement.h"

namespace paysphere::db {

IdempotencyRepository::IdempotencyRepository(DatabaseManager& database) : database_(database) {}

std::optional<IdempotencyRecord> IdempotencyRepository::find(std::int64_t userId, const std::string& key) {
    Statement query = database_.prepare(
        "SELECT request_fingerprint, transaction_id FROM idempotency_keys "
        "WHERE user_id = ? AND idempotency_key = ?",
        {userId, key});
    if (!query.step()) {
        return std::nullopt;
    }
    return IdempotencyRecord{query.columnText(0), query.columnInt64(1)};
}

void IdempotencyRepository::insert(std::int64_t userId, const std::string& key,
                                   const std::string& requestFingerprint, std::int64_t transactionId,
                                   const std::string& createdAt) {
    database_.insert(
        "INSERT INTO idempotency_keys (user_id, idempotency_key, request_fingerprint, transaction_id, "
        "created_at) VALUES (?, ?, ?, ?, ?)",
        {userId, key, requestFingerprint, transactionId, createdAt});
}

} // namespace paysphere::db
