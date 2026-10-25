#pragma once

#include "models/Errors.h"

#include <string>

namespace paysphere::db {

// A failure reported by the storage engine.
class DatabaseError : public PaySphereError {
public:
    DatabaseError(const std::string& message, int resultCode);

    const char* code() const noexcept override;

    // SQLite extended result code, e.g. 2067 for SQLITE_CONSTRAINT_UNIQUE.
    int resultCode() const noexcept { return resultCode_; }

private:
    int resultCode_;
};

// A UNIQUE, CHECK, NOT NULL, FOREIGN KEY or STRICT type rule rejected a write.
// Services catch this to turn, for example, a duplicate UPI ID into a clear
// message instead of a generic database failure.
class ConstraintViolation : public DatabaseError {
public:
    ConstraintViolation(const std::string& message, int resultCode);

    const char* code() const noexcept override;

    // True for UNIQUE or PRIMARY KEY violations (a duplicate value).
    bool isUniqueViolation() const noexcept;
};

// A compare-and-set update found the row changed (or gone) since it was read,
// so nothing was written. Not a lock timeout: retrying the same write would
// fail again; the caller must re-read and decide afresh.
class ConcurrentModificationError : public DatabaseError {
public:
    explicit ConcurrentModificationError(const std::string& message);

    const char* code() const noexcept override;
};

// A stored row breaks the rules the application relies on (e.g. an unknown
// status text). Indicates corruption or tampering, not bad user input.
class CorruptDataError : public DatabaseError {
public:
    explicit CorruptDataError(const std::string& message);

    const char* code() const noexcept override;
};

} // namespace paysphere::db
