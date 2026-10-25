#pragma once

#include "database/FailureSummary.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

class DatabaseManager;

// Persistence for the login_attempts table. Records outcomes only, never the
// password that was tried.
class LoginAttemptRepository {
public:
    explicit LoginAttemptRepository(DatabaseManager& database);

    // `identifier` is the lockout key chosen by AuthService. userId is
    // nullopt when the identifier matched no account.
    void record(const std::string& identifier, std::optional<std::int64_t> userId, bool succeeded,
                const std::string& attemptedAt);

    // Failures recorded under this identifier after `since` and after its
    // latest successful login (a success resets the count).
    FailureSummary recentFailures(const std::string& identifier, const std::string& since);

private:
    DatabaseManager& database_;
};

} // namespace paysphere::db
