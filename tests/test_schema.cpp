// Schema structure and constraints for every table.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/Statement.h"

#include <cstdint>
#include <string>
#include <vector>

using paysphere::db::ConstraintViolation;
using paysphere::db::DatabaseManager;
using paysphere::db::SqlParams;
using paysphere::db::SqlValue;
using paysphere::db::Statement;
using testsupport::insertDeposit;
using testsupport::insertUser;
using testsupport::insertWallet;

namespace {

// An initialized in-memory database.
struct Fixture {
    DatabaseManager db{":memory:"};
    Fixture() { db.initialize(); }

    std::int64_t user(const std::string& upiId, const std::string& phone) {
        return insertUser(db.connection(), upiId, phone);
    }
    std::int64_t count(const std::string& table) {
        return db.queryInt64("SELECT count(*) FROM " + table).value();
    }
    std::string queryPlan(const std::string& sql) {
        std::string plan;
        db.query("EXPLAIN QUERY PLAN " + sql, {},
                 [&plan](const Statement& row) { plan += row.columnText(3) + "\n"; });
        return plan;
    }
};

const std::string kInsertLoginAttempt =
    "INSERT INTO login_attempts (identifier, user_id, succeeded) VALUES (?, ?, ?)";
const std::string kInsertIdempotencyKey =
    "INSERT INTO idempotency_keys (user_id, idempotency_key, request_fingerprint, transaction_id) "
    "VALUES (?, ?, ?, ?)";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------- structure

TEST(schema_creates_all_tables_as_strict) {
    Fixture f;
    std::vector<std::string> tables;
    f.db.query("SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'table' "
               "AND name NOT LIKE 'sqlite_%' AND strict = 1 ORDER BY name",
               {}, [&tables](const Statement& row) { tables.push_back(row.columnText(0)); });
    const std::vector<std::string> expected{"idempotency_keys", "login_attempts", "transactions",
                                            "upi_pin_attempts", "upi_pins", "users", "wallets"};
    CHECK(tables == expected);
}

TEST(schema_declares_expected_foreign_keys) {
    Fixture f;
    auto foreignKeys = [&f](const std::string& table) {
        return f.db.queryInt64("SELECT count(*) FROM pragma_foreign_key_list(?)", {table}).value();
    };
    CHECK_EQ(foreignKeys("users"), 0);
    CHECK_EQ(foreignKeys("wallets"), 1);          // -> users
    CHECK_EQ(foreignKeys("transactions"), 2);     // -> wallets (from, to)
    CHECK_EQ(foreignKeys("login_attempts"), 1);   // -> users
    CHECK_EQ(foreignKeys("idempotency_keys"), 2); // -> users, transactions
    CHECK_EQ(foreignKeys("upi_pins"), 1);         // -> users
    CHECK_EQ(foreignKeys("upi_pin_attempts"), 1); // -> users
}

TEST(schema_passes_integrity_and_foreign_key_checks) {
    Fixture f;
    CHECK_EQ(f.db.queryText("PRAGMA integrity_check").value(), "ok");
    CHECK_EQ(f.db.query("PRAGMA foreign_key_check", {}, [](const Statement&) {}), 0u);
}

TEST(schema_indexes_serve_the_main_lookups) {
    Fixture f;
    // Lockout check: recent attempts for one identifier.
    CHECK(contains(f.queryPlan("SELECT count(*) FROM login_attempts "
                               "WHERE identifier = 'a' AND attempted_at >= '2026'"),
                   "idx_login_attempts_identifier"));
    // Duplicate-transfer check: has this user already used this key?
    CHECK(contains(f.queryPlan("SELECT transaction_id FROM idempotency_keys "
                               "WHERE user_id = 1 AND idempotency_key = 'k'"),
                   "sqlite_autoindex_idempotency_keys"));
    // Transaction history for a wallet.
    CHECK(contains(f.queryPlan("SELECT * FROM transactions WHERE from_wallet_id = 1 ORDER BY id DESC"),
                   "idx_transactions_from_wallet"));
    // Login lookup by UPI ID.
    CHECK(contains(f.queryPlan("SELECT id FROM users WHERE upi_id = 'a@paysphere'"),
                   "sqlite_autoindex_users"));
}

// -------------------------------------------------------- users and wallets

TEST(users_require_unique_phone_and_case_insensitive_upi_id) {
    Fixture f;
    f.user("asha@paysphere", "9876543210");
    CHECK_THROWS_AS(f.user("ASHA@PaySphere", "9876543211"), ConstraintViolation);
    CHECK_THROWS_AS(f.user("other@paysphere", "9876543210"), ConstraintViolation);

    const std::string insert =
        "INSERT INTO users (full_name, email, phone, upi_id, password_hash) VALUES (?, ?, ?, ?, ?)";
    const std::string email = "fresh@example.com";
    CHECK_THROWS_AS(f.db.execute(insert, {"", email, "9000000001", "x@paysphere", "h"}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute(insert, {std::string(101, 'a'), email, "9000000001", "x@paysphere", "h"}),
                    ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute(insert, {"Name", email, "9000000001", "x@paysphere", ""}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute(insert, {"Name", email, "9000000001", nullptr, "h"}), ConstraintViolation);
    // The same row is accepted once every rule is met, proving the failures
    // above came from the rule each one targets.
    f.db.execute(insert, {"Name", email, "9000000001", "x@paysphere", "h"});
    CHECK_EQ(f.count("users"), 2);
}

TEST(users_require_unique_case_insensitive_email) {
    Fixture f;
    insertUser(f.db.connection(), "asha@paysphere", "9876543210", "asha@example.com");
    CHECK_THROWS_AS(insertUser(f.db.connection(), "asha2@paysphere", "9876543211", "ASHA@Example.com"),
                    ConstraintViolation);
    CHECK_EQ(f.count("users"), 1);
}

TEST(users_require_email_on_insert_and_update) {
    Fixture f;
    CHECK_THROWS_AS(f.db.execute("INSERT INTO users (full_name, phone, upi_id, password_hash) "
                                 "VALUES ('Name', '9000000001', 'x@paysphere', 'h')"),
                    ConstraintViolation);
    const std::int64_t id = f.user("asha@paysphere", "9876543210");
    CHECK_THROWS_AS(f.db.execute("UPDATE users SET email = NULL WHERE id = ?", {id}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("UPDATE users SET email = 'ab' WHERE id = ?", {id}), ConstraintViolation);
    CHECK(f.db.queryText("SELECT email FROM users WHERE id = ?", {id}).has_value());
}

TEST(wallets_enforce_owner_balance_and_one_per_user) {
    Fixture f;
    const std::int64_t userId = f.user("asha@paysphere", "9876543210");
    CHECK_THROWS_AS(insertWallet(f.db.connection(), userId, -1), ConstraintViolation);
    CHECK_THROWS_AS(insertWallet(f.db.connection(), userId + 100, 0), ConstraintViolation);
    insertWallet(f.db.connection(), userId, 0);
    CHECK_THROWS_AS(insertWallet(f.db.connection(), userId, 0), ConstraintViolation);
}

TEST(money_columns_accept_only_integer_paise) {
    Fixture f;
    const std::int64_t walletId = insertWallet(f.db.connection(), f.user("a@paysphere", "9000000001"), 0);

    CHECK_THROWS_AS(f.db.execute("UPDATE wallets SET balance_paise = ?", {"100.50"}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("UPDATE wallets SET balance_paise = 100.5"), ConstraintViolation);
    CHECK_THROWS_AS(insertDeposit(f.db.connection(), walletId, 0, "PS0"), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("INSERT INTO transactions (reference, type, status, to_wallet_id, "
                                 "amount_paise) VALUES ('PS1', 'DEPOSIT', 'SUCCESS', ?, 9.99)",
                                 {walletId}),
                    ConstraintViolation);

    // Large amounts are exact: Rs. 9,99,99,99,999.99 survives a round trip.
    const std::int64_t large = INT64_C(99999999999);
    f.db.execute("UPDATE wallets SET balance_paise = ? WHERE id = ?", {large, walletId});
    CHECK_EQ(f.db.queryInt64("SELECT balance_paise FROM wallets WHERE id = ?", {walletId}).value(), large);
    CHECK_EQ(f.db.queryText("SELECT typeof(balance_paise) FROM wallets").value(), "integer");
}

TEST(users_with_wallets_cannot_be_deleted) {
    Fixture f;
    const std::int64_t userId = f.user("a@paysphere", "9000000001");
    insertWallet(f.db.connection(), userId, 0);
    CHECK_THROWS_AS(f.db.execute("DELETE FROM users WHERE id = ?", {userId}), ConstraintViolation);
}

// ------------------------------------------------------------ transactions

TEST(transactions_enforce_shape_by_type_and_status) {
    Fixture f;
    insertWallet(f.db.connection(), f.user("a@paysphere", "9000000001"), 0);
    insertWallet(f.db.connection(), f.user("b@paysphere", "9000000002"), 0);

    const std::string insert =
        "INSERT INTO transactions (reference, type, status, from_wallet_id, to_wallet_id, "
        "amount_paise, failure_reason) VALUES (?, ?, ?, ?, ?, ?, ?)";
    auto attempt = [&](const char* reference, const char* type, const char* status, SqlValue from,
                       SqlValue to, std::int64_t amount, SqlValue reason) {
        f.db.execute(insert, {reference, type, status, from, to, amount, reason});
    };
    const SqlValue none = nullptr;
    const SqlValue w1 = std::int64_t{1};
    const SqlValue w2 = std::int64_t{2};

    attempt("PS1", "DEPOSIT", "SUCCESS", none, w1, 500, none);
    attempt("PS2", "TRANSFER", "SUCCESS", w1, w2, 500, none);
    attempt("PS3", "TRANSFER", "FAILED", w1, none, 500, "Recipient not found");

    CHECK_THROWS_AS(attempt("PS1", "DEPOSIT", "SUCCESS", none, w1, 500, none), ConstraintViolation);  // duplicate reference
    CHECK_THROWS_AS(attempt("PS5", "DEPOSIT", "SUCCESS", w2, w1, 500, none), ConstraintViolation);    // deposit with source
    CHECK_THROWS_AS(attempt("PS6", "TRANSFER", "SUCCESS", w1, w1, 500, none), ConstraintViolation);   // self transfer
    CHECK_THROWS_AS(attempt("PS7", "TRANSFER", "SUCCESS", w1, none, 500, none), ConstraintViolation); // success without recipient
    CHECK_THROWS_AS(attempt("PS8", "TRANSFER", "FAILED", w1, w2, 500, none), ConstraintViolation);    // failed, no reason
    CHECK_THROWS_AS(attempt("PS9", "TRANSFER", "SUCCESS", w1, w2, 500, "x"), ConstraintViolation);    // success with reason
    CHECK_THROWS_AS(attempt("PSA", "REFUND", "SUCCESS", none, w1, 500, none), ConstraintViolation);   // unknown type
    CHECK_THROWS_AS(attempt("PSB", "DEPOSIT", "SUCCESS", none, std::int64_t{99}, 500, none),
                    ConstraintViolation); // no such wallet
}

// ---------------------------------------------------------- login_attempts

TEST(login_attempts_record_known_and_unknown_identifiers) {
    Fixture f;
    const std::int64_t userId = f.user("asha@paysphere", "9876543210");
    f.db.insert(kInsertLoginAttempt, {"asha@paysphere", userId, std::int64_t{0}});
    f.db.insert(kInsertLoginAttempt, {"asha@paysphere", userId, std::int64_t{1}});
    f.db.insert(kInsertLoginAttempt, {"nobody@paysphere", nullptr, std::int64_t{0}}); // unknown account
    CHECK_EQ(f.count("login_attempts"), 3);

    // Identifier lookups ignore case, so "ASHA@..." counts against the same account.
    CHECK_EQ(f.db.queryInt64("SELECT count(*) FROM login_attempts WHERE identifier = ? AND succeeded = 0",
                             {"ASHA@PAYSPHERE"}).value(),
             1);
    CHECK(!f.db.queryText("SELECT attempted_at FROM login_attempts LIMIT 1").value().empty());
}

TEST(login_attempts_reject_invalid_rows) {
    Fixture f;
    const std::int64_t userId = f.user("asha@paysphere", "9876543210");
    CHECK_THROWS_AS(f.db.insert(kInsertLoginAttempt, {"asha@paysphere", userId, std::int64_t{2}}),
                    ConstraintViolation); // succeeded must be 0 or 1
    CHECK_THROWS_AS(f.db.insert(kInsertLoginAttempt, {"", userId, std::int64_t{0}}),
                    ConstraintViolation); // empty identifier
    CHECK_THROWS_AS(f.db.insert(kInsertLoginAttempt, {std::string(101, 'x'), userId, std::int64_t{0}}),
                    ConstraintViolation); // identifier too long
    CHECK_THROWS_AS(f.db.insert(kInsertLoginAttempt, {"x", userId + 50, std::int64_t{0}}),
                    ConstraintViolation); // no such user
    CHECK_THROWS_AS(f.db.insert(kInsertLoginAttempt, {"x", userId, "yes"}),
                    ConstraintViolation); // STRICT: text in an integer column
}

TEST(login_attempts_are_removed_with_their_user) {
    Fixture f;
    const std::int64_t userId = f.user("asha@paysphere", "9876543210");
    f.db.insert(kInsertLoginAttempt, {"asha@paysphere", userId, std::int64_t{1}});
    f.db.execute("DELETE FROM users WHERE id = ?", {userId});
    CHECK_EQ(f.count("login_attempts"), 0);
}

// -------------------------------------------------------- idempotency_keys

TEST(idempotency_keys_are_unique_per_user) {
    Fixture f;
    const std::int64_t alice = f.user("alice@paysphere", "9000000001");
    const std::int64_t bob = f.user("bob@paysphere", "9000000002");
    const std::int64_t wallet = insertWallet(f.db.connection(), alice, 0);
    const std::int64_t first = insertDeposit(f.db.connection(), wallet, 100, "PS1");
    const std::int64_t second = insertDeposit(f.db.connection(), wallet, 100, "PS2");
    const std::int64_t third = insertDeposit(f.db.connection(), wallet, 100, "PS3");

    f.db.insert(kInsertIdempotencyKey, {alice, "key-001", "fp", first});
    // Same user, same key: rejected, so a retried request cannot pay twice.
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "key-001", "fp", second}),
                    ConstraintViolation);
    // Keys are scoped to a user: Bob may use the same key text.
    f.db.insert(kInsertIdempotencyKey, {bob, "key-001", "fp", second});
    // One transaction is produced by at most one key.
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "key-002", "fp", first}),
                    ConstraintViolation);
    f.db.insert(kInsertIdempotencyKey, {alice, "key-002", "fp", third});

    CHECK_EQ(f.db.queryInt64("SELECT transaction_id FROM idempotency_keys "
                             "WHERE user_id = ? AND idempotency_key = ?",
                             {alice, "key-001"}).value(),
             first);
}

TEST(idempotency_keys_reject_invalid_rows) {
    Fixture f;
    const std::int64_t alice = f.user("alice@paysphere", "9000000001");
    const std::int64_t wallet = insertWallet(f.db.connection(), alice, 0);
    const std::int64_t txn = insertDeposit(f.db.connection(), wallet, 100, "PS1");

    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "", "fp", txn}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, std::string(129, 'k'), "fp", txn}),
                    ConstraintViolation);
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "k", "", txn}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "k", "fp", nullptr}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice, "k", "fp", txn + 100}),
                    ConstraintViolation); // no such transaction
    CHECK_THROWS_AS(f.db.insert(kInsertIdempotencyKey, {alice + 100, "k", "fp", txn}),
                    ConstraintViolation); // no such user
    CHECK_EQ(f.count("idempotency_keys"), 0);
}

TEST(foreign_keys_declare_the_intended_delete_actions) {
    // Checked from the schema itself: since v5 the append-only triggers fire
    // before any foreign-key action could be observed on transactions.
    Fixture f;
    auto onDelete = [&f](const std::string& table, const std::string& parent) {
        return f.db.queryText("SELECT on_delete FROM pragma_foreign_key_list(?) WHERE \"table\" = ?",
                              {table, parent}).value();
    };
    CHECK_EQ(onDelete("idempotency_keys", "transactions"), "RESTRICT");
    CHECK_EQ(onDelete("idempotency_keys", "users"), "CASCADE");
    CHECK_EQ(onDelete("wallets", "users"), "RESTRICT");
    CHECK_EQ(onDelete("login_attempts", "users"), "CASCADE");
}

TEST(transactions_are_append_only) {
    Fixture f;
    const std::int64_t alice = f.user("alice@paysphere", "9000000001");
    const std::int64_t txn = insertDeposit(f.db.connection(), insertWallet(f.db.connection(), alice, 0), 100, "PS1");
    f.db.insert(kInsertIdempotencyKey, {alice, "key-001", "fp", txn});

    CHECK_THROWS_AS(f.db.execute("UPDATE transactions SET status = 'FAILED', failure_reason = 'x' WHERE id = ?",
                                 {txn}),
                    ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("UPDATE transactions SET amount_paise = 1 WHERE id = ?", {txn}),
                    ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("DELETE FROM transactions WHERE id = ?", {txn}), ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("UPDATE idempotency_keys SET transaction_id = 999"), ConstraintViolation);
    // No idempotency key refers to this one, so only the trigger protects it.
    const std::int64_t unkeyed = insertDeposit(f.db.connection(), 1, 200, "PS2");
    CHECK_THROWS_AS(f.db.execute("DELETE FROM transactions WHERE id = ?", {unkeyed}), ConstraintViolation);
    CHECK_EQ(f.count("transactions"), 2);
    CHECK_EQ(f.db.queryText("SELECT status FROM transactions WHERE id = ?", {txn}).value(), "SUCCESS");
    CHECK_EQ(f.db.queryInt64("SELECT amount_paise FROM transactions WHERE id = ?", {txn}).value(), 100);
}
