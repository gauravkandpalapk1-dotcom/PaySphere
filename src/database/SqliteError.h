#pragma once

// Private to the database layer: translates SQLite result codes into the
// DatabaseError hierarchy.

#include <string>

struct sqlite3;

namespace paysphere::db::detail {

// Throws ConstraintViolation for SQLITE_CONSTRAINT*, DatabaseError otherwise.
[[noreturn]] void throwError(int resultCode, const std::string& message);

// Like throwError, appending the connection's latest error message.
[[noreturn]] void throwLastError(sqlite3* handle, int resultCode, const std::string& context);

} // namespace paysphere::db::detail
