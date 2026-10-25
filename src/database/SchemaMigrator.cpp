#include "database/SchemaMigrator.h"

#include "database/Database.h"
#include "database/DatabaseError.h"
#include "database/DbTransaction.h"
#include "database/Statement.h"

#include <sqlite3.h>

#include <iterator>
#include <string>

namespace paysphere::db {

namespace {

struct Migration {
    int version;
    const char* sql;
};

// STRICT tables reject values of the wrong type (e.g. text in a money column).
// Money is INTEGER paise; timestamps are UTC ISO-8601 text.
constexpr Migration kMigrations[] = {
    {1, R"SQL(
        CREATE TABLE users (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            full_name     TEXT    NOT NULL CHECK (length(full_name) BETWEEN 1 AND 100),
            phone         TEXT    NOT NULL UNIQUE,
            upi_id        TEXT    NOT NULL UNIQUE COLLATE NOCASE,
            password_hash TEXT    NOT NULL CHECK (length(password_hash) > 0),
            created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
        ) STRICT;

        CREATE TABLE wallets (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id       INTEGER NOT NULL UNIQUE REFERENCES users (id) ON DELETE RESTRICT,
            balance_paise INTEGER NOT NULL DEFAULT 0 CHECK (balance_paise >= 0),
            created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
        ) STRICT;

        CREATE TABLE transactions (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            reference      TEXT    NOT NULL UNIQUE,
            type           TEXT    NOT NULL CHECK (type IN ('DEPOSIT', 'TRANSFER')),
            status         TEXT    NOT NULL CHECK (status IN ('SUCCESS', 'FAILED')),
            from_wallet_id INTEGER REFERENCES wallets (id) ON DELETE RESTRICT,
            to_wallet_id   INTEGER REFERENCES wallets (id) ON DELETE RESTRICT,
            amount_paise   INTEGER NOT NULL CHECK (amount_paise > 0),
            failure_reason TEXT,
            created_at     TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
            CHECK ((status = 'FAILED') = (failure_reason IS NOT NULL)),
            CHECK (type <> 'DEPOSIT'
                   OR (from_wallet_id IS NULL AND to_wallet_id IS NOT NULL)),
            CHECK (type <> 'TRANSFER'
                   OR (from_wallet_id IS NOT NULL
                       AND (to_wallet_id IS NOT NULL OR status = 'FAILED')
                       AND from_wallet_id IS NOT to_wallet_id))
        ) STRICT;

        CREATE INDEX idx_transactions_from_wallet ON transactions (from_wallet_id, id);
        CREATE INDEX idx_transactions_to_wallet   ON transactions (to_wallet_id, id);
    )SQL"},

    // login_attempts: audit trail for brute-force protection. It records the
    // identifier the person typed and the outcome, never the password.
    // user_id is NULL when the identifier matched no account.
    //
    // idempotency_keys: duplicate-transfer prevention. A client sends a
    // unique key with each payment request; a retry with the same key finds
    // this row and returns the original transaction instead of paying twice.
    // request_fingerprint detects a key reused for a different request.
    {2, R"SQL(
        CREATE TABLE login_attempts (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            identifier   TEXT    NOT NULL COLLATE NOCASE
                                 CHECK (length(identifier) BETWEEN 1 AND 100),
            user_id      INTEGER REFERENCES users (id) ON DELETE CASCADE,
            succeeded    INTEGER NOT NULL CHECK (succeeded IN (0, 1)),
            attempted_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
        ) STRICT;

        CREATE INDEX idx_login_attempts_identifier ON login_attempts (identifier, attempted_at);
        CREATE INDEX idx_login_attempts_user       ON login_attempts (user_id, attempted_at);

        CREATE TABLE idempotency_keys (
            id                  INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id             INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
            idempotency_key     TEXT    NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 128),
            request_fingerprint TEXT    NOT NULL CHECK (length(request_fingerprint) BETWEEN 1 AND 128),
            transaction_id      INTEGER NOT NULL UNIQUE
                                        REFERENCES transactions (id) ON DELETE RESTRICT,
            created_at          TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
            UNIQUE (user_id, idempotency_key)
        ) STRICT;

        CREATE INDEX idx_idempotency_keys_created ON idempotency_keys (created_at);
    )SQL"},

    // Email becomes the primary login identifier. SQLite cannot add a NOT
    // NULL column to a table that may already hold rows, so the column is
    // nullable for any pre-existing users and triggers require it on every
    // new or updated row. Uniqueness ignores case, like upi_id.
    {3, R"SQL(
        ALTER TABLE users ADD COLUMN email TEXT COLLATE NOCASE
            CHECK (email IS NULL OR length(email) BETWEEN 3 AND 254);

        CREATE UNIQUE INDEX idx_users_email ON users (email);

        CREATE TRIGGER users_email_required_on_insert
        BEFORE INSERT ON users WHEN NEW.email IS NULL
        BEGIN
            SELECT RAISE(ABORT, 'users.email is required');
        END;

        CREATE TRIGGER users_email_required_on_update
        BEFORE UPDATE OF email ON users WHEN NEW.email IS NULL
        BEGIN
            SELECT RAISE(ABORT, 'users.email is required');
        END;
    )SQL"},

    // UPI PINs authorize payments. Only an Argon2id hash is stored. Attempts
    // are logged so that repeated wrong PINs block payments for a while.
    {4, R"SQL(
        CREATE TABLE upi_pins (
            user_id    INTEGER PRIMARY KEY REFERENCES users (id) ON DELETE CASCADE,
            pin_hash   TEXT    NOT NULL CHECK (length(pin_hash) > 0),
            created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
        ) STRICT;

        CREATE TABLE upi_pin_attempts (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id      INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
            succeeded    INTEGER NOT NULL CHECK (succeeded IN (0, 1)),
            attempted_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
        ) STRICT;

        CREATE INDEX idx_upi_pin_attempts_user ON upi_pin_attempts (user_id, attempted_at);
    )SQL"},

    // The audit trail is append-only. A recorded transaction (including its
    // status) can never be changed or deleted, even with raw SQL, and the
    // key-to-transaction mapping that prevents duplicate payments cannot be
    // re-pointed.
    {5, R"SQL(
        CREATE TRIGGER transactions_no_update
        BEFORE UPDATE ON transactions
        BEGIN
            SELECT RAISE(ABORT, 'transactions are append-only and cannot be modified');
        END;

        CREATE TRIGGER transactions_no_delete
        BEFORE DELETE ON transactions
        BEGIN
            SELECT RAISE(ABORT, 'transactions are append-only and cannot be deleted');
        END;

        CREATE TRIGGER idempotency_keys_no_update
        BEFORE UPDATE ON idempotency_keys
        BEGIN
            SELECT RAISE(ABORT, 'idempotency keys cannot be modified');
        END;
    )SQL"},

    // Audit fixes from the Phase 8 review:
    //   - payee_upi_id keeps the UPI ID a payer entered, so a FAILED transfer
    //     to an unknown recipient still shows who was meant to be paid;
    //   - idempotency keys can no longer be deleted (and then re-inserted to
    //     point at a different transaction).
    {6, R"SQL(
        ALTER TABLE transactions ADD COLUMN payee_upi_id TEXT
            CHECK (payee_upi_id IS NULL OR length(payee_upi_id) BETWEEN 3 AND 100);

        CREATE TRIGGER idempotency_keys_no_delete
        BEFORE DELETE ON idempotency_keys
        BEGIN
            SELECT RAISE(ABORT, 'idempotency keys cannot be deleted');
        END;
    )SQL"},
};

static_assert(kMigrations[std::size(kMigrations) - 1].version == SchemaMigrator::kLatestVersion,
              "kLatestVersion must match the last migration");

} // namespace

SchemaMigrator::SchemaMigrator(Database& database) : database_(database) {}

int SchemaMigrator::currentVersion() const {
    Statement query = database_.prepare("PRAGMA user_version");
    if (!query.step()) {
        throw DatabaseError("Cannot read schema version", SQLITE_ERROR);
    }
    return static_cast<int>(query.columnInt64(0));
}

int SchemaMigrator::migrate(int targetVersion) {
    if (targetVersion < 1 || targetVersion > kLatestVersion) {
        throw DatabaseError("Unknown target schema version " + std::to_string(targetVersion),
                            SQLITE_MISUSE);
    }

    // Take the write lock before reading the version, so two processes cannot
    // both decide to apply the same migration.
    DbTransaction transaction(database_);

    const int current = currentVersion();
    if (current > kLatestVersion) {
        throw DatabaseError("Database schema version " + std::to_string(current) +
                                " is newer than this build supports (" +
                                std::to_string(kLatestVersion) + ")",
                            SQLITE_ERROR);
    }

    int applied = 0;
    for (const Migration& migration : kMigrations) {
        if (migration.version <= current || migration.version > targetVersion) {
            continue;
        }
        database_.execute(migration.sql);
        // user_version is stored in the database header, so it commits or
        // rolls back together with the migration itself.
        database_.execute("PRAGMA user_version = " + std::to_string(migration.version));
        ++applied;
    }

    transaction.commit();
    return applied;
}

} // namespace paysphere::db
