#pragma once

#include "database/FailureSummary.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

class DatabaseManager;

// Persistence for upi_pins (one hashed PIN per user) and upi_pin_attempts.
class UpiPinRepository {
public:
    explicit UpiPinRepository(DatabaseManager& database);

    // Throws ConstraintViolation if the user already has a PIN.
    void insert(std::int64_t userId, const std::string& pinHash);
    std::optional<std::string> findHash(std::int64_t userId);

    void recordAttempt(std::int64_t userId, bool succeeded, const std::string& attemptedAt);
    // Failed attempts after `since` and after the latest successful one.
    FailureSummary recentFailures(std::int64_t userId, const std::string& since);

private:
    DatabaseManager& database_;
};

} // namespace paysphere::db
