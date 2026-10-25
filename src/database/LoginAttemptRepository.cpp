#include "database/LoginAttemptRepository.h"

#include "database/DatabaseManager.h"
#include "database/Statement.h"

namespace paysphere::db {

LoginAttemptRepository::LoginAttemptRepository(DatabaseManager& database) : database_(database) {}

void LoginAttemptRepository::record(const std::string& identifier, std::optional<std::int64_t> userId,
                                    bool succeeded, const std::string& attemptedAt) {
    database_.insert(
        "INSERT INTO login_attempts (identifier, user_id, succeeded, attempted_at) VALUES (?, ?, ?, ?)",
        {identifier, userId ? SqlValue(*userId) : SqlValue(nullptr),
         std::int64_t{succeeded ? 1 : 0}, attemptedAt});
}

FailureSummary LoginAttemptRepository::recentFailures(const std::string& identifier, const std::string& since) {
    // Ids increase with time, so "after the latest success" compares ids;
    // this stays exact even when attempts share the same second.
    Statement query = database_.prepare(
        "SELECT count(*), max(attempted_at) FROM login_attempts "
        "WHERE identifier = ?1 AND succeeded = 0 AND attempted_at > ?2 "
        "AND id > coalesce((SELECT max(id) FROM login_attempts "
        "                   WHERE identifier = ?1 AND succeeded = 1), 0)",
        {identifier, since});
    FailureSummary summary;
    if (query.step()) {
        summary.failures = query.columnInt64(0);
        if (!query.columnIsNull(1)) {
            summary.lastFailureAt = query.columnText(1);
        }
    }
    return summary;
}

} // namespace paysphere::db
