#include "database/DatabaseManager.h"

#include "database/DatabaseError.h"
#include "database/SchemaMigrator.h"

#include <sqlite3.h>

#include <filesystem>
#include <system_error>

namespace paysphere::db {

namespace {

constexpr const char* kInMemoryPath = ":memory:";

// Lets the first run work on a fresh checkout without creating data/ by hand.
void ensureParentDirectory(const std::string& path) {
    if (path == kInMemoryPath) {
        return;
    }
    const std::filesystem::path parent = std::filesystem::u8path(path).parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        throw DatabaseError("Cannot create directory '" + parent.u8string() + "': " + error.message(),
                            SQLITE_CANTOPEN);
    }
}

} // namespace

DatabaseManager::DatabaseManager(std::string path, std::chrono::milliseconds busyTimeout)
    : path_(std::move(path)), busyTimeout_(busyTimeout) {
    if (path_.empty()) {
        throw DatabaseError("Database path must not be empty", SQLITE_MISUSE);
    }
}

void DatabaseManager::initialize() {
    if (database_) {
        return;
    }
    ensureParentDirectory(path_);

    Database database(path_, busyTimeout_);
    SchemaMigrator migrator(database);
    const int applied = migrator.migrate();
    const int version = migrator.currentVersion();

    database_.emplace(std::move(database));
    migrationsApplied_ = applied;
    schemaVersion_ = version;
}

void DatabaseManager::close() {
    if (!database_) {
        return;
    }
    if (database_->inTransaction()) {
        throw DatabaseError("Cannot close the database while a transaction is in progress",
                            SQLITE_BUSY);
    }
    database_.reset();
}

Database& DatabaseManager::connection() {
    if (!database_) {
        throw DatabaseError("Database is not open; call initialize() first", SQLITE_MISUSE);
    }
    return *database_;
}

Statement DatabaseManager::prepare(const std::string& sql, const SqlParams& params) {
    Statement statement = connection().prepare(sql);
    statement.bindAll(params);
    return statement;
}

int DatabaseManager::execute(const std::string& sql, const SqlParams& params) {
    Statement statement = prepare(sql, params);
    // Checked before running, so a SELECT is refused without side effects.
    if (statement.columnCount() > 0) {
        throw DatabaseError("execute() is for statements without results; use query()",
                            SQLITE_MISUSE);
    }
    statement.step();
    return connection().changes();
}

std::int64_t DatabaseManager::insert(const std::string& sql, const SqlParams& params) {
    if (execute(sql, params) != 1) {
        throw DatabaseError("INSERT did not add exactly one row", SQLITE_MISMATCH);
    }
    return connection().lastInsertRowId();
}

std::size_t DatabaseManager::query(const std::string& sql, const SqlParams& params,
                                   const RowHandler& onRow) {
    Statement statement = prepare(sql, params);
    std::size_t rows = 0;
    while (statement.step()) {
        onRow(statement);
        ++rows;
    }
    return rows;
}

std::optional<std::int64_t> DatabaseManager::queryInt64(const std::string& sql,
                                                        const SqlParams& params) {
    Statement statement = prepare(sql, params);
    if (!statement.step() || statement.columnIsNull(0)) {
        return std::nullopt;
    }
    return statement.columnInt64(0);
}

std::optional<std::string> DatabaseManager::queryText(const std::string& sql,
                                                      const SqlParams& params) {
    Statement statement = prepare(sql, params);
    if (!statement.step() || statement.columnIsNull(0)) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

} // namespace paysphere::db
