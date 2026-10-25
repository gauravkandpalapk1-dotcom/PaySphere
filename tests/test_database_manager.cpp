// DatabaseManager: initialization, connection management, parameterized
// queries and transactions.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/Database.h"
#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "database/Statement.h"
#include "services/ApplicationContext.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using paysphere::db::ConstraintViolation;
using paysphere::db::Database;
using paysphere::db::DatabaseError;
using paysphere::db::DatabaseManager;
using paysphere::db::SchemaMigrator;
using paysphere::db::Statement;
using paysphere::services::ApplicationContext;
using testsupport::TempDirectory;

namespace {

const std::string kInsertUser =
    "INSERT INTO users (full_name, email, phone, upi_id, password_hash) VALUES (?, ?, ?, ?, ?)";

std::int64_t addUser(DatabaseManager& db, const std::string& upiId, const std::string& phone) {
    return db.insert(kInsertUser, {"Test User", testsupport::uniqueEmail(), phone, upiId, "test-hash"});
}

std::int64_t userCount(DatabaseManager& db) {
    return db.queryInt64("SELECT count(*) FROM users").value();
}

} // namespace

// ------------------------------------------------- initialization/lifecycle

TEST(manager_rejects_empty_path) {
    CHECK_THROWS_AS(DatabaseManager(""), DatabaseError);
}

TEST(manager_is_unusable_until_initialized) {
    DatabaseManager db(":memory:");
    CHECK(!db.isOpen());
    CHECK_EQ(db.schemaVersion(), 0);
    CHECK_THROWS_AS(db.connection(), DatabaseError);
    CHECK_THROWS_AS(db.queryInt64("SELECT 1"), DatabaseError);
}

TEST(manager_initialize_migrates_and_is_idempotent) {
    DatabaseManager db(":memory:");
    db.initialize();
    CHECK(db.isOpen());
    CHECK_EQ(db.schemaVersion(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(db.migrationsApplied(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(db.queryInt64("PRAGMA foreign_keys").value(), 1);

    addUser(db, "asha@paysphere", "9876543210");
    db.initialize(); // already open: must not reopen or lose data
    CHECK_EQ(userCount(db), 1);
}

TEST(manager_close_and_reopen_keeps_file_data) {
    TempDirectory directory;
    const std::string path = directory.file("nested/dir/paysphere.db");
    DatabaseManager db(path);
    db.initialize();
    CHECK(std::filesystem::exists(std::filesystem::u8path(path)));
    CHECK_EQ(db.queryText("PRAGMA journal_mode").value(), "wal");
    addUser(db, "asha@paysphere", "9876543210");

    db.close();
    CHECK(!db.isOpen());
    db.close(); // closing twice is harmless

    db.initialize();
    CHECK_EQ(db.migrationsApplied(), 0);
    CHECK_EQ(userCount(db), 1);
}

TEST(manager_upgrades_existing_phase1_file) {
    TempDirectory directory;
    const std::string path = directory.file("phase1.db");
    {
        Database legacy(path);
        SchemaMigrator migrator(legacy);
        migrator.migrate(1);
        testsupport::insertLegacyUser(legacy, "asha@paysphere", "9876543210");
    }

    DatabaseManager db(path);
    db.initialize();
    CHECK_EQ(db.migrationsApplied(), SchemaMigrator::kLatestVersion - 1);
    CHECK_EQ(db.schemaVersion(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(userCount(db), 1);
    CHECK_EQ(db.queryInt64("SELECT count(*) FROM login_attempts").value(), 0);
}

TEST(manager_stays_closed_when_initialization_fails) {
    TempDirectory directory;
    const std::string path = directory.file("future.db");
    {
        Database future(path);
        future.execute("PRAGMA user_version = 99");
    }
    DatabaseManager db(path);
    CHECK_THROWS_AS(db.initialize(), DatabaseError);
    CHECK(!db.isOpen());
}

TEST(application_context_initializes_its_database) {
    TempDirectory directory;
    ApplicationContext context(directory.file("data/paysphere.db"));
    CHECK(context.database().isOpen());
    CHECK_EQ(context.database().schemaVersion(), SchemaMigrator::kLatestVersion);
}

// ---------------------------------------------------- parameterized queries

TEST(manager_execute_and_insert_report_results) {
    DatabaseManager db(":memory:");
    db.initialize();
    const std::int64_t first = addUser(db, "a@paysphere", "9000000001");
    const std::int64_t second = addUser(db, "b@paysphere", "9000000002");
    CHECK(second > first);

    CHECK_EQ(db.execute("UPDATE users SET full_name = ? WHERE id >= ?", {"Renamed", first}), 2);
    CHECK_EQ(db.execute("DELETE FROM users WHERE id = ?", {std::int64_t{999}}), 0);
    CHECK_THROWS_AS(db.execute("SELECT * FROM users"), DatabaseError);   // has result rows
    CHECK_THROWS_AS(db.execute("DELETE FROM users WHERE id = ?"), DatabaseError); // missing parameter
    CHECK_EQ(userCount(db), 2);
}

TEST(manager_query_helpers_handle_rows_and_nulls) {
    DatabaseManager db(":memory:");
    db.initialize();
    addUser(db, "b@paysphere", "9000000002");
    addUser(db, "a@paysphere", "9000000001");

    std::vector<std::string> upiIds;
    const std::size_t rows = db.query("SELECT upi_id FROM users WHERE phone LIKE ? ORDER BY upi_id",
                                      {"9%"},
                                      [&upiIds](const Statement& row) { upiIds.push_back(row.columnText(0)); });
    CHECK_EQ(rows, 2u);
    CHECK(upiIds == (std::vector<std::string>{"a@paysphere", "b@paysphere"}));

    CHECK(!db.queryInt64("SELECT id FROM users WHERE upi_id = ?", {"nobody"}).has_value());
    CHECK(!db.queryInt64("SELECT sum(balance_paise) FROM wallets").has_value()); // SUM of nothing is NULL
    CHECK(!db.queryText("SELECT NULL").has_value());
    CHECK_EQ(db.queryText("SELECT full_name FROM users WHERE upi_id = ?", {"A@PAYSPHERE"}).value(),
             "Test User");
}

TEST(manager_parameters_are_data_not_sql) {
    DatabaseManager db(":memory:");
    db.initialize();
    const std::string hostile = "' OR '1'='1";
    addUser(db, "real@paysphere", "9000000001");
    CHECK(!db.queryInt64("SELECT id FROM users WHERE upi_id = ?", {hostile}).has_value());

    const std::string dropper = "x'); DROP TABLE users; --";
    addUser(db, dropper, "9000000002");
    CHECK_EQ(db.queryText("SELECT upi_id FROM users WHERE phone = ?", {"9000000002"}).value(), dropper);
    CHECK_EQ(userCount(db), 2);
}

// ------------------------------------------------------------- transactions

TEST(manager_transaction_commits_and_returns_result) {
    DatabaseManager db(":memory:");
    db.initialize();
    const std::int64_t id = db.inTransaction([&] { return addUser(db, "a@paysphere", "9000000001"); });
    CHECK(id > 0);
    CHECK(!db.connection().inTransaction());
    CHECK_EQ(userCount(db), 1);
}

TEST(manager_transaction_rolls_back_every_write_on_failure) {
    DatabaseManager db(":memory:");
    db.initialize();

    // A constraint failure part-way through undoes the earlier insert too.
    CHECK_THROWS_AS(db.inTransaction([&] {
                        const std::int64_t userId = addUser(db, "a@paysphere", "9000000001");
                        db.insert("INSERT INTO wallets (user_id, balance_paise) VALUES (?, ?)",
                                  {userId, std::int64_t{10000}});
                        addUser(db, "A@PaySphere", "9000000002"); // duplicate UPI ID
                    }),
                    ConstraintViolation);
    CHECK_EQ(userCount(db), 0);
    CHECK_EQ(db.queryInt64("SELECT count(*) FROM wallets").value(), 0);

    // Any exception type triggers the rollback.
    CHECK_THROWS_AS(db.inTransaction([&] {
                        addUser(db, "a@paysphere", "9000000001");
                        throw std::runtime_error("simulated crash");
                    }),
                    std::runtime_error);
    CHECK_EQ(userCount(db), 0);
    CHECK(!db.connection().inTransaction());
}

TEST(manager_transaction_is_not_reentrant) {
    DatabaseManager db(":memory:");
    db.initialize();
    CHECK_THROWS_AS(db.inTransaction([&] {
                        addUser(db, "a@paysphere", "9000000001");
                        db.inTransaction([] {});
                    }),
                    DatabaseError);
    CHECK_EQ(userCount(db), 0); // the outer transaction rolled back as well
}

TEST(manager_refuses_to_close_during_transaction) {
    DatabaseManager db(":memory:");
    db.initialize();
    db.inTransaction([&] {
        addUser(db, "a@paysphere", "9000000001");
        CHECK_THROWS_AS(db.close(), DatabaseError);
        CHECK(db.isOpen());
    });
    CHECK_EQ(userCount(db), 1);
}
