#pragma once

#include "database/Database.h"
#include "database/DbTransaction.h"
#include "database/SqlValue.h"
#include "database/Statement.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace paysphere::db {

// The single entry point that services use to reach the database.
//
// Responsibilities:
//   - initialization: create the data directory, open the connection
//     (foreign keys enforced) and apply pending schema migrations;
//   - connection management: initialize(), close(), isOpen();
//   - parameterized queries: every value is bound to a "?" placeholder, never
//     concatenated into SQL;
//   - transactions: inTransaction() commits when the work returns and rolls
//     back when it throws.
//
// It is an ordinary object passed to services through their constructors,
// not a global singleton, so each test can use its own in-memory database.
// It is neither copyable nor movable, because Statements and DbTransactions
// refer to the connection it owns.
//
// Threading: one DatabaseManager is one SQLite connection and must be used
// by one thread at a time (SQLite transactions belong to the connection, so
// two threads sharing it would share a transaction). Concurrency between
// *processes* is safe: each has its own connection, and BEGIN IMMEDIATE plus
// the busy timeout serialize their writes.
class DatabaseManager {
public:
    using RowHandler = std::function<void(const Statement&)>;

    // Does not touch the disk; call initialize() to open the database.
    // ":memory:" gives a private in-memory database that is discarded on close().
    explicit DatabaseManager(std::string path,
                             std::chrono::milliseconds busyTimeout = Database::kDefaultBusyTimeout);

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;
    DatabaseManager(DatabaseManager&&) = delete;
    DatabaseManager& operator=(DatabaseManager&&) = delete;

    // Opens the database and brings its schema up to date. The connection is
    // published only after migration succeeds. Does nothing if already open.
    void initialize();
    // Closes the connection. Throws DatabaseError while a transaction is open.
    void close();
    bool isOpen() const noexcept { return database_.has_value(); }

    const std::string& path() const noexcept { return path_; }
    // Schema version after the last initialize().
    int schemaVersion() const noexcept { return schemaVersion_; }
    // Migrations applied by the last initialize() (0 if the schema was current).
    int migrationsApplied() const noexcept { return migrationsApplied_; }

    // The open connection. Throws DatabaseError before initialize().
    Database& connection();

    Statement prepare(const std::string& sql, const SqlParams& params = {});

    // Runs one INSERT, UPDATE or DELETE and returns the number of rows changed.
    // Rejects SQL that produces result rows (use query()).
    int execute(const std::string& sql, const SqlParams& params = {});
    // Runs one INSERT of exactly one row and returns its id. Throws
    // DatabaseError if no row was inserted (e.g. INSERT OR IGNORE skipped it),
    // rather than returning the id of some earlier row.
    std::int64_t insert(const std::string& sql, const SqlParams& params = {});

    // Calls onRow for each result row and returns the number of rows.
    std::size_t query(const std::string& sql, const SqlParams& params, const RowHandler& onRow);
    // First column of the first row; nullopt if there is no row or it is NULL.
    std::optional<std::int64_t> queryInt64(const std::string& sql, const SqlParams& params = {});
    std::optional<std::string> queryText(const std::string& sql, const SqlParams& params = {});

    // Runs work() atomically and returns its result. If work() throws, every
    // change it made is rolled back and the exception propagates. Not
    // re-entrant: starting a transaction inside another throws DatabaseError.
    template <typename Work>
    auto inTransaction(Work&& work) {
        return run(DbTransaction::Mode::Write, std::forward<Work>(work));
    }

    // Runs read-only work on one consistent snapshot without taking the
    // write lock, so it does not block payments.
    template <typename Work>
    auto inReadTransaction(Work&& work) {
        return run(DbTransaction::Mode::Read, std::forward<Work>(work));
    }

private:
    template <typename Work>
    auto run(DbTransaction::Mode mode, Work&& work) {
        DbTransaction transaction(connection(), mode);
        if constexpr (std::is_void_v<std::invoke_result_t<Work&&>>) {
            std::forward<Work>(work)();
            transaction.commit();
        } else {
            auto result = std::forward<Work>(work)();
            transaction.commit();
            return result;
        }
    }

    std::string path_;
    std::chrono::milliseconds busyTimeout_;
    std::optional<Database> database_;
    int schemaVersion_ = 0;
    int migrationsApplied_ = 0;
};

} // namespace paysphere::db
