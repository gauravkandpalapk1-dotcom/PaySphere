// Low-level persistence layer: Database, Statement, DbTransaction, SchemaMigrator.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/Database.h"
#include "database/DatabaseError.h"
#include "database/DbTransaction.h"
#include "database/SchemaMigrator.h"
#include "database/Statement.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

using paysphere::db::ConstraintViolation;
using paysphere::db::Database;
using paysphere::db::DatabaseError;
using paysphere::db::DbTransaction;
using paysphere::db::SchemaMigrator;
using paysphere::db::SqlParams;
using paysphere::db::Statement;
using testsupport::insertUser;
using testsupport::insertWallet;
using testsupport::migratedDatabase;
using testsupport::scalar;

TEST(database_enables_foreign_keys) {
    Database db(":memory:");
    CHECK_EQ(scalar(db, "PRAGMA foreign_keys"), 1);
}

TEST(database_is_movable_and_rejects_use_after_move) {
    Database first(":memory:");
    Database second(std::move(first));
    CHECK(!first.isOpen());
    CHECK(second.isOpen());
    CHECK_THROWS_AS(first.execute("SELECT 1"), DatabaseError);
    CHECK_EQ(scalar(second, "SELECT 42"), 42);
}

TEST(migrator_creates_schema_once) {
    Database db(":memory:");
    SchemaMigrator migrator(db);
    CHECK_EQ(migrator.currentVersion(), 0);
    CHECK_EQ(migrator.migrate(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(migrator.currentVersion(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(migrator.migrate(), 0);
}

TEST(migrator_upgrades_phase1_database_without_losing_data) {
    Database db(":memory:");
    SchemaMigrator migrator(db);
    CHECK_EQ(migrator.migrate(1), 1);
    testsupport::insertLegacyUser(db, "asha@paysphere", "9876543210");
    CHECK_EQ(scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name = 'login_attempts'"), 0);

    CHECK_EQ(migrator.migrate(), SchemaMigrator::kLatestVersion - 1);
    CHECK_EQ(migrator.currentVersion(), SchemaMigrator::kLatestVersion);
    CHECK_EQ(scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name = 'login_attempts'"), 1);
    // The pre-existing user survives; it simply has no email yet.
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users WHERE email IS NULL"), 1);
}

TEST(migrator_refuses_newer_schema_and_unknown_targets) {
    Database db(":memory:");
    SchemaMigrator migrator(db);
    CHECK_THROWS_AS(migrator.migrate(0), DatabaseError);
    CHECK_THROWS_AS(migrator.migrate(SchemaMigrator::kLatestVersion + 1), DatabaseError);
    db.execute("PRAGMA user_version = 99");
    CHECK_THROWS_AS(migrator.migrate(), DatabaseError);
}

TEST(statement_binds_and_reads_values) {
    Database db = migratedDatabase();
    const std::int64_t id = insertUser(db, "asha@paysphere", "9876543210");
    CHECK(id > 0);

    Statement query = db.prepare("SELECT upi_id, phone, created_at FROM users WHERE id = ?");
    query.bind(1, id);
    CHECK(query.step());
    CHECK_EQ(query.columnText(0), "asha@paysphere");
    CHECK_EQ(query.columnText(1), "9876543210");
    CHECK(!query.columnText(2).empty());
    CHECK(!query.step());

    // Bound text is data, never SQL.
    const std::string hostile = "x'); DROP TABLE users; --";
    insertUser(db, hostile, "9876543211");
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users"), 2);
}

TEST(statement_bind_all_requires_exact_parameter_count) {
    Database db(":memory:");
    Statement statement = db.prepare("SELECT ? + ?");
    CHECK_THROWS_AS(statement.bindAll(SqlParams{std::int64_t{1}}), DatabaseError);
    CHECK_THROWS_AS(statement.bindAll(SqlParams{std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}),
                    DatabaseError);
    statement.bindAll(SqlParams{std::int64_t{40}, std::int64_t{2}});
    CHECK(statement.step());
    CHECK_EQ(statement.columnInt64(0), 42);

    Statement nullable = db.prepare("SELECT ? IS NULL");
    nullable.bindAll(SqlParams{nullptr});
    CHECK(nullable.step());
    CHECK_EQ(nullable.columnInt64(0), 1);
}

TEST(statement_checks_column_types_and_indices) {
    Database db = migratedDatabase();
    insertUser(db, "asha@paysphere", "9876543210");
    Statement query = db.prepare("SELECT upi_id FROM users");
    CHECK_THROWS_AS(query.columnText(0), DatabaseError); // no row yet
    CHECK(query.step());
    CHECK_THROWS_AS(query.columnInt64(0), DatabaseError); // text, not integer
    CHECK_THROWS_AS(query.columnText(1), DatabaseError);  // out of range
}

TEST(prepare_rejects_invalid_or_multiple_statements) {
    Database db(":memory:");
    CHECK_THROWS_AS(db.prepare("SELEC 1"), DatabaseError);
    CHECK_THROWS_AS(db.prepare("SELECT 1; SELECT 2"), DatabaseError);
    CHECK_THROWS_AS(db.prepare("   "), DatabaseError);
    CHECK_EQ(scalar(db, "SELECT 1;"), 1);
}

TEST(db_transaction_rolls_back_unless_committed) {
    Database db = migratedDatabase();
    {
        DbTransaction transaction(db);
        CHECK(db.inTransaction());
        insertUser(db, "a@paysphere", "9000000001");
    } // no commit
    CHECK(!db.inTransaction());
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users"), 0);

    try {
        DbTransaction transaction(db);
        insertUser(db, "a@paysphere", "9000000001");
        throw std::runtime_error("simulated failure mid-operation");
    } catch (const std::runtime_error&) {
    }
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users"), 0);

    {
        DbTransaction transaction(db);
        insertUser(db, "a@paysphere", "9000000001");
        transaction.commit();
        CHECK(!transaction.isActive());
        CHECK_THROWS_AS(transaction.commit(), DatabaseError);
    }
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users"), 1);
}

TEST(db_transaction_undoes_all_writes_after_constraint_failure) {
    Database db = migratedDatabase();
    try {
        DbTransaction transaction(db);
        insertWallet(db, insertUser(db, "a@paysphere", "9000000001"), 100);
        insertUser(db, "A@PAYSPHERE", "9000000002"); // duplicate UPI ID
        transaction.commit();
    } catch (const ConstraintViolation&) {
    }
    CHECK_EQ(scalar(db, "SELECT count(*) FROM users"), 0);
    CHECK_EQ(scalar(db, "SELECT count(*) FROM wallets"), 0);
}
