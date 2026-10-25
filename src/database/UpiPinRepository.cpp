#include "database/UpiPinRepository.h"

#include "database/DatabaseManager.h"
#include "database/Statement.h"

namespace paysphere::db {

UpiPinRepository::UpiPinRepository(DatabaseManager& database) : database_(database) {}

void UpiPinRepository::insert(std::int64_t userId, const std::string& pinHash) {
    database_.insert("INSERT INTO upi_pins (user_id, pin_hash) VALUES (?, ?)", {userId, pinHash});
}

std::optional<std::string> UpiPinRepository::findHash(std::int64_t userId) {
    return database_.queryText("SELECT pin_hash FROM upi_pins WHERE user_id = ?", {userId});
}

void UpiPinRepository::recordAttempt(std::int64_t userId, bool succeeded, const std::string& attemptedAt) {
    database_.insert("INSERT INTO upi_pin_attempts (user_id, succeeded, attempted_at) VALUES (?, ?, ?)",
                     {userId, std::int64_t{succeeded ? 1 : 0}, attemptedAt});
}

FailureSummary UpiPinRepository::recentFailures(std::int64_t userId, const std::string& since) {
    Statement query = database_.prepare(
        "SELECT count(*), max(attempted_at) FROM upi_pin_attempts "
        "WHERE user_id = ?1 AND succeeded = 0 AND attempted_at > ?2 "
        "AND id > coalesce((SELECT max(id) FROM upi_pin_attempts "
        "                   WHERE user_id = ?1 AND succeeded = 1), 0)",
        {userId, since});
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
