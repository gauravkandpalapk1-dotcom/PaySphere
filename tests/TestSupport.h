#pragma once

// Helpers shared by the database test files.

#include "database/Database.h"
#include "database/SchemaMigrator.h"
#include "database/Statement.h"
#include "services/Clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace testsupport {

inline paysphere::db::Database migratedDatabase() {
    paysphere::db::Database db(":memory:");
    paysphere::db::SchemaMigrator migrator(db);
    migrator.migrate();
    return db;
}

inline std::int64_t scalar(paysphere::db::Database& db, const std::string& sql) {
    paysphere::db::Statement query = db.prepare(sql);
    if (!query.step()) {
        throw std::runtime_error("query returned no rows: " + sql);
    }
    return query.columnInt64(0);
}

// A fresh email for each call, so tests about UPI ID or phone uniqueness
// cannot pass merely because the email collided.
inline std::string uniqueEmail() {
    static std::atomic<int> counter{0};
    return "user" + std::to_string(++counter) + "@example.com";
}

// Inserts a user into the current schema (email required since v3).
inline std::int64_t insertUser(paysphere::db::Database& db, const std::string& upiId,
                               const std::string& phone, const std::string& email = uniqueEmail()) {
    paysphere::db::Statement insert = db.prepare(
        "INSERT INTO users (full_name, email, phone, upi_id, password_hash) VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, std::string("Test User")).bind(2, email).bind(3, phone).bind(4, upiId)
        .bind(5, std::string("test-hash"));
    insert.step();
    return db.lastInsertRowId();
}

// Inserts a user into a schema older than v3, which has no email column.
inline std::int64_t insertLegacyUser(paysphere::db::Database& db, const std::string& upiId,
                                     const std::string& phone) {
    paysphere::db::Statement insert = db.prepare(
        "INSERT INTO users (full_name, phone, upi_id, password_hash) VALUES (?, ?, ?, ?)");
    insert.bind(1, std::string("Legacy User")).bind(2, phone).bind(3, upiId).bind(4, std::string("test-hash"));
    insert.step();
    return db.lastInsertRowId();
}

inline std::int64_t insertWallet(paysphere::db::Database& db, std::int64_t userId,
                                 std::int64_t balancePaise) {
    paysphere::db::Statement insert =
        db.prepare("INSERT INTO wallets (user_id, balance_paise) VALUES (?, ?)");
    insert.bind(1, userId).bind(2, balancePaise);
    insert.step();
    return db.lastInsertRowId();
}

inline std::int64_t insertDeposit(paysphere::db::Database& db, std::int64_t walletId,
                                  std::int64_t amountPaise, const std::string& reference) {
    paysphere::db::Statement insert = db.prepare(
        "INSERT INTO transactions (reference, type, status, to_wallet_id, amount_paise) "
        "VALUES (?, 'DEPOSIT', 'SUCCESS', ?, ?)");
    insert.bind(1, reference).bind(2, walletId).bind(3, amountPaise);
    insert.step();
    return db.lastInsertRowId();
}

// A clock that only moves when a test tells it to.
class ManualClock final : public paysphere::services::Clock {
public:
    explicit ManualClock(const std::string& start = "2026-10-14T10:00:00Z")
        : now_(paysphere::services::parseTimestamp(start)) {}

    paysphere::services::TimePoint now() const override { return now_; }
    void advance(std::chrono::seconds duration) { now_ += duration; }

private:
    paysphere::services::TimePoint now_;
};

// A unique directory under the system temp folder, deleted on scope exit.
class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("paysphere-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string file(const std::string& name) const { return (path_ / name).u8string(); }

private:
    std::filesystem::path path_;
};

} // namespace testsupport
