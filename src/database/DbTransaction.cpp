#include "database/DbTransaction.h"

#include "database/Database.h"
#include "database/DatabaseError.h"

#include <sqlite3.h>

namespace paysphere::db {

DbTransaction::DbTransaction(Database& database, Mode mode) : database_(database) {
    database_.execute(mode == Mode::Write ? "BEGIN IMMEDIATE" : "BEGIN DEFERRED");
    active_ = true;
}

DbTransaction::~DbTransaction() {
    if (active_) {
        try {
            database_.execute("ROLLBACK");
        } catch (...) {
            // Destructors must not throw. SQLite also rolls back on its own
            // after some errors, in which case there is nothing left to undo.
        }
    }
}

void DbTransaction::commit() {
    requireActive();
    database_.execute("COMMIT");
    active_ = false; // only once COMMIT succeeded; otherwise the destructor rolls back
}

void DbTransaction::rollback() {
    requireActive();
    database_.execute("ROLLBACK");
    active_ = false;
}

void DbTransaction::requireActive() const {
    if (!active_) {
        throw DatabaseError("Transaction has already been committed or rolled back", SQLITE_MISUSE);
    }
}

} // namespace paysphere::db
