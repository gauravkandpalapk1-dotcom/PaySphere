#include "database/Database.h"

#include "SqliteError.h"
#include "database/DatabaseError.h"
#include "database/Statement.h"

#include <sqlite3.h>

#include <utility>

namespace paysphere::db {

namespace {

// STRICT tables, used by the schema, arrived in SQLite 3.37.0.
constexpr int kMinimumSqliteVersion = 3037000;
constexpr const char* kInMemoryPath = ":memory:";

} // namespace

Database::Database(const std::string& path, std::chrono::milliseconds busyTimeout) : path_(path) {
    if (path_.empty()) {
        throw DatabaseError("Database path must not be empty", SQLITE_MISUSE);
    }
    if (sqlite3_libversion_number() < kMinimumSqliteVersion) {
        throw DatabaseError(std::string("SQLite 3.37.0 or newer is required, found ") +
                                sqlite3_libversion(),
                            SQLITE_ERROR);
    }

    sqlite3* handle = nullptr;
    const int rc = sqlite3_open_v2(path_.c_str(), &handle,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        const std::string detail = handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(rc);
        sqlite3_close(handle); // safe on nullptr; frees a partially opened handle
        throw DatabaseError("Cannot open database '" + path_ + "': " + detail, rc);
    }
    handle_ = handle;

    try {
        configure(busyTimeout);
    } catch (...) {
        close();
        throw;
    }
}

Database::~Database() {
    close();
}

Database::Database(Database&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void Database::configure(std::chrono::milliseconds busyTimeout) {
    sqlite3_extended_result_codes(handle_, 1);
    sqlite3_busy_timeout(handle_, static_cast<int>(busyTimeout.count()));

    // Enabled per connection (SQLite's historical default is off). Read it
    // back, because the PRAGMA is silently ignored if unsupported.
    execute("PRAGMA foreign_keys = ON");
    Statement check = prepare("PRAGMA foreign_keys");
    if (!check.step() || check.columnInt64(0) != 1) {
        throw DatabaseError("SQLite foreign key enforcement is unavailable", SQLITE_ERROR);
    }

    if (path_ != kInMemoryPath) {
        execute("PRAGMA journal_mode = WAL");
    }
}

void Database::close() noexcept {
    if (handle_ != nullptr) {
        // close_v2 defers the close if a Statement is still alive, instead of failing.
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

sqlite3* Database::requireOpen() const {
    if (handle_ == nullptr) {
        throw DatabaseError("Database connection is not open", SQLITE_MISUSE);
    }
    return handle_;
}

void Database::execute(const std::string& sql) {
    sqlite3* handle = requireOpen();
    char* message = nullptr;
    const int rc = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : sqlite3_errstr(rc);
        sqlite3_free(message);
        detail::throwError(rc, "SQL execution failed: " + detail);
    }
}

Statement Database::prepare(const std::string& sql) {
    return Statement(*this, sql);
}

std::int64_t Database::lastInsertRowId() const {
    return sqlite3_last_insert_rowid(requireOpen());
}

int Database::changes() const {
    return sqlite3_changes(requireOpen());
}

bool Database::inTransaction() const noexcept {
    return handle_ != nullptr && sqlite3_get_autocommit(handle_) == 0;
}

const char* Database::libraryVersion() noexcept {
    return sqlite3_libversion();
}

} // namespace paysphere::db
