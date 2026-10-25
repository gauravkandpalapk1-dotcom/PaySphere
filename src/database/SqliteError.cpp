#include "SqliteError.h"

#include "database/DatabaseError.h"

#include <sqlite3.h>

namespace paysphere::db::detail {

void throwError(int resultCode, const std::string& message) {
    // The low byte of an extended result code is the primary code.
    if ((resultCode & 0xff) == SQLITE_CONSTRAINT) {
        throw ConstraintViolation(message, resultCode);
    }
    throw DatabaseError(message, resultCode);
}

void throwLastError(sqlite3* handle, int resultCode, const std::string& context) {
    const char* detail = handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(resultCode);
    throwError(resultCode, context + ": " + detail);
}

} // namespace paysphere::db::detail
