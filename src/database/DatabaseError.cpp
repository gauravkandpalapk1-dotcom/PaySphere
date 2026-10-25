#include "database/DatabaseError.h"

#include <sqlite3.h>

namespace paysphere::db {

DatabaseError::DatabaseError(const std::string& message, int resultCode)
    : PaySphereError(message), resultCode_(resultCode) {}

const char* DatabaseError::code() const noexcept {
    return "DATABASE_ERROR";
}

ConstraintViolation::ConstraintViolation(const std::string& message, int resultCode)
    : DatabaseError(message, resultCode) {}

const char* ConstraintViolation::code() const noexcept {
    return "CONSTRAINT_VIOLATION";
}

bool ConstraintViolation::isUniqueViolation() const noexcept {
    return resultCode() == SQLITE_CONSTRAINT_UNIQUE || resultCode() == SQLITE_CONSTRAINT_PRIMARYKEY;
}

ConcurrentModificationError::ConcurrentModificationError(const std::string& message)
    : DatabaseError(message, SQLITE_ABORT) {}

const char* ConcurrentModificationError::code() const noexcept {
    return "CONCURRENT_MODIFICATION";
}

CorruptDataError::CorruptDataError(const std::string& message)
    : DatabaseError(message, SQLITE_CORRUPT) {}

const char* CorruptDataError::code() const noexcept {
    return "CORRUPT_DATA";
}

} // namespace paysphere::db
