#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// Forward declaration keeps sqlite3.h out of every file that includes this one.
struct sqlite3;

namespace paysphere::db {

class Statement;

// Owns one SQLite connection (RAII: opened in the constructor, closed in the
// destructor). Move-only, because two owners would close the handle twice.
//
// Every connection enforces foreign keys and waits briefly on lock contention.
// File-backed databases use write-ahead logging.
class Database {
public:
    // How long a statement waits for another connection's lock before
    // failing with SQLITE_BUSY ("database is locked").
    static constexpr std::chrono::milliseconds kDefaultBusyTimeout{5000};

    // Opens or creates the database at path (UTF-8). ":memory:" gives a
    // private in-memory database, which the tests use.
    explicit Database(const std::string& path, std::chrono::milliseconds busyTimeout = kDefaultBusyTimeout);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Runs SQL that takes no parameters and returns no rows needed by the
    // caller (schema, PRAGMAs, BEGIN/COMMIT). Never pass user input here:
    // use prepare() with bound parameters instead.
    void execute(const std::string& sql);

    // Compiles exactly one SQL statement for parameter binding.
    Statement prepare(const std::string& sql);

    std::int64_t lastInsertRowId() const;
    // Rows changed by the most recent INSERT, UPDATE or DELETE.
    int changes() const;

    const std::string& path() const noexcept { return path_; }
    bool isOpen() const noexcept { return handle_ != nullptr; }
    // True between BEGIN and COMMIT/ROLLBACK.
    bool inTransaction() const noexcept;

    // Version of the linked SQLite library, e.g. "3.54.0".
    static const char* libraryVersion() noexcept;

private:
    friend class Statement;

    void configure(std::chrono::milliseconds busyTimeout);
    void close() noexcept;
    sqlite3* requireOpen() const;

    sqlite3* handle_ = nullptr;
    std::string path_;
};

} // namespace paysphere::db
