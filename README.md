# PaySphere: Digital Wallet & UPI Payment Simulator

PaySphere is an object-oriented C++17 backend for a digital wallet with
UPI-style person-to-person payments. It uses SQLite for persistence and
libsodium for cryptography, and comes with a minimal command-line front end
for demonstration. It is an educational PBL project.

> **Simulation only.** All accounts and money are fictional. PaySphere never
> contacts a bank, NPCI or any real UPI system, and must not be used with
> real financial data.

## Contents

1. [Features](#features)
2. [Prerequisites](#prerequisites)
3. [Setup](#setup)
4. [Building](#building)
5. [Running](#running)
6. [Architecture](#architecture)
7. [OOP concepts](#oop-concepts)
8. [Security design](#security-design)
9. [Money, consistency and auditability](#money-consistency-and-auditability)
10. [Database schema](#database-schema)
11. [Testing and results](#testing-and-results)
12. [Phase 8 QA audit](#phase-8-qa-audit)
13. [Known limitations](#known-limitations)
14. [Project history](#project-history)
15. [Third-party components](#third-party-components)

## Features

| Area | What it does |
|---|---|
| **Accounts** | Registration with an email, a UPI ID, an Indian mobile number and a password. Each user gets a wallet, created atomically with the account. |
| **Login** | Login by email or UPI ID. Passwords are hashed with Argon2id. Five failures within 15 minutes trigger a 15-minute lockout. |
| **Sessions** | 256-bit random tokens with a 30-minute idle timeout and a 12-hour absolute limit. Logout ends the session. |
| **Wallets** | Balance checks and simulated deposits. A wallet holds at most ₹2,00,000. |
| **UPI payments** | Pay by UPI ID, authorized by a UPI PIN. 3 wrong PINs block payments for 24 hours. A risk engine applies limits on payment frequency, daily totals and new payees. |
| **Duplicate protection** | Idempotency keys on deposits and payments. A retried request never moves money twice. |
| **History** | Paginated transaction history with sent/received and status filters, search by transaction ID, and receipts with times in IST and UTC. |
| **Integrity** | Every balance change is a single database transaction. Transaction records are append-only, and a ledger audit compares balances with the transaction record. |

## Prerequisites

| Tool | Version used and tested | Notes |
|---|---|---|
| C++ compiler | **GCC 16.2.0**, WinLibs MinGW-w64 UCRT, 64-bit | Any C++17 compiler should work (GCC 8+, Clang 7+, MSVC 2019+), but only GCC 16.2.0 was tested |
| CMake | **4.4.3** | 3.16 or newer is required |
| Ninja | **1.13.2** | Other generators should work, but only Ninja was tested |
| OS | **Windows 11** | Linux and macOS build files are provided but **untested** |

Bundled in `third_party/`, so nothing needs downloading on Windows x64:
* **SQLite 3.54.0**, compiled from the amalgamation. It is public domain.
* **libsodium 1.0.22** as a static 64-bit MinGW library. It is ISC-licensed,
  and its release signature was checked when it was added; see
  `third_party/libsodium/README.md`.

On Linux or macOS, install libsodium and pkg-config first:
`apt install libsodium-dev pkg-config` or `brew install libsodium pkg-config`.

## Setup

1. Install a C++17 toolchain with CMake and Ninja. On Windows, the WinLibs
   GCC package includes all three. This project used
   `C:\Users\GAURAV\Tools\winlibs\mingw64\bin`.
2. Put the toolchain first on `PATH` for the current shell. An older MinGW
   (such as GCC 6.3 in `C:\MinGW`) cannot compile C++17, so it must not be
   picked up first:

   ```powershell
   $env:PATH = "C:\Users\GAURAV\Tools\winlibs\mingw64\bin;$env:PATH"
   ```
3. Nothing else is needed: both libraries are vendored. The database file is
   created and migrated automatically on first run.

## Building

### Windows (tested)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

MinGW executables are linked statically, so `build\paysphere.exe` runs
without compiler DLLs on `PATH`. It depends only on DLLs that ship with
Windows.

### Linux / macOS (untested)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build options

| Option | Default | Effect |
|---|---|---|
| `PAYSPHERE_BUILD_TESTS` | `ON` | Build `paysphere_tests` and register both CTest suites |
| `PAYSPHERE_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX`. The code builds cleanly with it on. |
| `PAYSPHERE_USE_SYSTEM_SQLITE` | `OFF` | Link an installed SQLite ≥ 3.37 instead. The hardening flags compiled into the bundled copy (`SQLITE_DQS=0`, `SQLITE_OMIT_LOAD_EXTENSION`) then don't apply. |
| `PAYSPHERE_USE_BUNDLED_SODIUM` | `ON` on 64-bit MinGW | Otherwise libsodium is found through pkg-config |

The code is compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual`.

## Running

```powershell
.\build\paysphere.exe                  # interactive menu on data\paysphere.db
.\build\paysphere.exe --db demo.db     # use another database file
.\build\paysphere.exe --help
Get-Content .\tests\cli\session1.txt | .\build\paysphere.exe --db demo.db   # replay a scripted demo
```

The default database path is fixed at build time to `<source>/data/paysphere.db`.
Use `--db` for any other location.

| Logged out | Logged in |
|---|---|
| 1 Register · 2 Login · 0 Exit | 1 Balance · 2 Deposit · 3 Pay by UPI ID · 4 History (all/sent/received) · 5 Receipt by transaction ID · 6 Set UPI PIN · 7 Logout · 0 Exit |

Before your first payment, set a UPI PIN (option 6).

**How the CLI behaves:**
* **Secrets.** Passwords and PINs are typed with terminal echo turned off.
  When input is piped from a script, each line is echoed so the transcript is
  readable, but secrets always appear as `****`.
* **Errors.** Every error is reported and the menu continues.
* **Expired sessions.** An expired session returns you to the logged-out menu.
* **End of input.** Ctrl+Z on Windows, Ctrl+D elsewhere, or the end of a
  script logs you out and exits cleanly.

```
Choose: 3
Payee UPI ID: ravi@paysphere
Amount in rupees: 250.50
UPI PIN: ****
Paid Rs. 250.50 to ravi@paysphere. Transaction ID TRF20261014D2871DB5181C
...
Showing 3 of 3 transaction(s), newest first:
DATE (IST)                    TYPE      COUNTERPARTY            AMOUNT            STATUS   ID
14 Oct 2026, 11:58:39 AM IST  Sent      ravi@paysphere          Rs. 5,000.00      FAILED   TRF2026...
14 Oct 2026, 11:58:39 AM IST  Sent      ravi@paysphere          Rs. 250.50        SUCCESS  TRF2026...
14 Oct 2026, 11:58:39 AM IST  Deposit   (bank, simulated)       Rs. 1,000.00      SUCCESS  DEP2026...
```

## Architecture

### Layers and directory layout

```
include/ and src/ (same structure)
├── models/     Domain entities: User, Wallet, Transaction, Session,
│               TransactionDetails; the exception hierarchy. No SQL, no I/O.
├── payments/   Money (integer paise), UpiId, PaymentMethod (abstract),
│               UPIPayment, SecretString, CredentialVerifier (interface)
├── database/   Database, Statement, DbTransaction, DatabaseManager,
│               SchemaMigrator, and one repository per table group
└── services/   Business rules: AuthService, SessionManager, WalletService,
                PaymentService, RiskEngine, UpiPinService, TransactionService,
                IdempotencyLedger, InputValidator, Clock, PasswordHasher,
                ApplicationContext (the composition root)
src/main.cpp    CLI front end: input and output only
tests/          Unit, integration, end-to-end and QA suites; tests/cli/ holds
                the scripted CLI test
data/           Runtime database (git-ignored)
third_party/    SQLite and libsodium
```

Dependencies point one way:

```
main.cpp ──► services ──► database ──► models ◄── payments
                 │                          ▲
                 └──────────► payments ─────┘
```

`payments` never depends on `services`. It declares the
`CredentialVerifier` interface it needs, and `UpiPinService` implements it.

Public headers include neither `sqlite3.h` nor `sodium.h`, so only the
database and services `.cpp` files depend on those libraries.

### Components

| Component | Responsibility |
|---|---|
| `ApplicationContext` | Composition root. Builds every object once, in dependency order, and injects collaborators through constructors. `ApplicationOptions` lets tests supply a manual clock, a cheaper hashing cost and a short lock timeout. |
| `DatabaseManager` | Owns one SQLite connection. Provides `initialize`/`close`, parameterized `execute`/`insert`/`query`, `inTransaction` (write lock) and `inReadTransaction` (snapshot). |
| Repositories | All SQL lives here: `User`, `Wallet`, `Transaction`, `LoginAttempt`, `UpiPin` and `Idempotency` repositories. Services see only models. |
| `AuthService` + `SessionManager` | Registration, login, lockout, logout and session validation |
| `WalletService` | The caller's wallet, balance and deposits |
| `PaymentService` + `UpiPinService` + `RiskEngine` | UPI transfers, the PIN, and fraud and limit rules |
| `TransactionService` + `Receipt` | Read-only history, search, receipts and ledger audit |
| `IdempotencyLedger` | Duplicate-request protection shared by deposits and payments |

### Life of a payment (`PaymentService::pay`)

1. **Authenticate** the session token.
2. **Validate** the idempotency key and the amount (₹1 to ₹1,00,000). If this
   exact request was seen before, return its stored result.
3. **Check eligibility:**
   * the payer isn't paying themselves;
   * a PIN is set and not blocked;
   * the payer hasn't had 10 or more failed attempts in the last 10 minutes.
4. **Resolve the payee and verify the PIN.** The PIN check is slow Argon2id
   work, so it runs *before* the write lock is taken.
5. **`BEGIN IMMEDIATE`** takes the write lock. Then:
   1. Re-check the idempotency key and the PIN block, so parallel requests
      can't exceed the limits.
   2. Decide the outcome, checked in this order: unknown recipient → wrong
      PIN → risk rules → insufficient balance → recipient's wallet full.
   3. On success, compare-and-set debit the sender, then compare-and-set
      credit the receiver.
   4. Insert the transaction record and the idempotency key.
   5. `COMMIT`.
6. **Any exception rolls back every change.** That covers both balances, the
   record, the key and the PIN attempt.

## OOP concepts

| Concept | Where to look |
|---|---|
| **Classes and objects** | Every domain concept is a class: `User`, `Wallet`, `Transaction`, `Money`, `UpiId`, the services. Objects are composed in `ApplicationContext`. |
| **Encapsulation** | `Wallet` has no setters. Its balance rules (`canAfford`, `canReceive`, `headroom`) live with the data, and `WalletRepository::updateBalance` is the only code that writes a balance. `User` doesn't even carry the password hash. `Money` hides its paise. |
| **Abstraction** | `PaymentMethod`, `PasswordHasher`, `Clock`, `ReferenceGenerator`, `RiskRule` and `CredentialVerifier` are abstract interfaces. `Database`/`Statement` hide the SQLite C API. |
| **Inheritance** | `UPIPayment : PaymentMethod`. `SodiumPasswordHasher : PasswordHasher`. `SystemClock : Clock`. `VelocityRule`, `DailyLimitRule`, `NewPayeeRule : RiskRule`. The exception tree `PaySphereError` → `ValidationError`, `AuthenticationError` → `AccountLockedError` / `SessionError`, `DatabaseError` → `ConstraintViolation` / `ConcurrentModificationError` / `CorruptDataError`, and more. |
| **Polymorphism** | `PaymentService::pay` accepts any `PaymentMethod&`. `RiskEngine` runs `std::vector<std::unique_ptr<RiskRule>>`, and new rules plug in with `addRule` (Strategy pattern, open/closed). Errors report their category through a virtual `code()`. Tests substitute `ManualClock`, a scripted `ReferenceGenerator` and racing test doubles. |
| **Constructors and RAII** | Constructors validate invariants, so `Money`, `UpiId`, `Transaction` and `Wallet` can't exist in an invalid state. `Database`, `Statement` and `DbTransaction` release their resources in destructors, and `DbTransaction` rolls back when it is not committed. `SecretString` wipes its memory on destruction. |
| **Exception handling** | A typed hierarchy with stable codes. Services throw; the CLI catches by category. Repository corruption surfaces as `CorruptDataError`, not as user-input errors. |
| **Dependency injection** | Every service receives its collaborators through its constructor. There are no singletons or globals. |
| **File separation** | One class or cohesive group per `.h`/`.cpp` pair. `include/` mirrors `src/`. |

## Security design

* **Passwords.**
  * Hashed with **Argon2id** through libsodium `crypto_pwhash_str`, at
    64 MiB and 2 passes, with a random salt per hash.
  * Hashes made with weaker parameters are upgraded at the next successful login.
  * There is no custom cryptography, and plaintext is never stored.
  * The rules follow NIST SP 800-63B: 8–128 characters, a list of common
    passwords, and no name, email, UPI handle or phone inside the password.
* **Login.**
  * The same message is given for "no such account" and "wrong password".
  * Exactly one Argon2id verification runs on every path (a precomputed
    dummy hash covers unknown accounts), so timing doesn't reveal which
    accounts exist.
  * Lockout is counted **per identifier typed** and re-checked under the
    write lock (see the audit).
  * Unknown identifiers are stored only as a BLAKE2b digest. This guards
    against a password typed into the email field ending up in the logs.
* **Sessions.**
  * Tokens are 32 bytes from libsodium's CSPRNG. Only their BLAKE2b hashes
    are kept.
  * Sessions end after 30 minutes idle or 12 hours in total, or at logout.
* **Authorization.**
  * Every service call starts by authenticating the token, and operates only
    on the caller's own wallet.
  * "Not yours" and "doesn't exist" give identical errors, so IDs can't be
    probed.
  * A failed transfer is visible only to the payer, never to the intended
    recipient.
* **UPI PIN.**
  * 4 or 6 digits, set once, with trivial PINs rejected and only an Argon2id
    hash stored.
  * 3 wrong PINs block payments for 24 hours, and the check is exact even
    under parallel requests.
* **Risk rules.**
  * At most 5 successful transfers per 10 minutes.
  * At most ₹1,00,000 per rolling 24 hours.
  * ₹20,000 maximum per payment to a payee **for 24 hours after the first
    payment** to them, so a ₹1 test payment doesn't lift the cap.
  * At most 10 failed attempts per 10 minutes.
* **SQL injection.**
  * Every value is a bound parameter.
  * `prepare()` rejects multiple statements.
  * `bindAll()` rejects a wrong parameter count.
* **Secrets in memory.** `SecretString` and `secureWipe` overwrite PINs and
  passwords (with `sodium_memzero`) when they go out of scope, including when
  a constructor throws.

## Money, consistency and auditability

* **Integer paise everywhere.** `Money` wraps `int64_t` paise with
  overflow-checked arithmetic and Indian digit grouping (`Rs. 12,34,567.89`).
  The money columns are `INTEGER` in `STRICT` tables, so `100.5` or `"100.50"`
  is rejected by SQLite itself.
* **Atomicity.** Each deposit or transfer is one `BEGIN IMMEDIATE`
  transaction, managed by RAII `DbTransaction`. The balance writes, the
  transaction record and the idempotency key commit together or not at all.
* **Compare-and-set balances.** `UPDATE … WHERE id = ? AND balance_paise =
  <value read>`. A stale decision fails with `ConcurrentModificationError`.
  `CHECK (balance_paise >= 0)` forbids negative balances even through raw SQL.
* **Idempotency.**
  * `UNIQUE (user_id, idempotency_key)` stores a BLAKE2b fingerprint of the
    request with each key.
  * Repeating a request returns the original transaction.
  * Reusing a key for a different request raises `IdempotencyConflictError`.
  * Keys never expire. They can't be updated or deleted (schema v5 and v6
    triggers).
* **Append-only audit trail.** Triggers reject `UPDATE` and `DELETE` on
  `transactions`. Failed attempts are recorded too, with a reason and the
  UPI ID the payer entered.
* **Reconciliation.** `TransactionService::audit` recomputes a balance from
  the ledger, inside one read snapshot, and compares it with the stored
  balance. A test proves it detects a balance edited directly in the database.

## Database schema

The schema is at **version 6**. Every table is `STRICT`, uses `INTEGER
PRIMARY KEY AUTOINCREMENT`, stores UTC ISO-8601 timestamps from the
application clock, and has foreign keys enforced on every connection.

| Table | Purpose | Keys, constraints, indexes |
|---|---|---|
| `users` | Account holders | UNIQUE `email`, UNIQUE `upi_id` (both case-insensitive), UNIQUE `phone`. `password_hash` non-empty. Name 1–100 characters. Triggers require an email on every new or updated row. |
| `wallets` | One per user | `user_id` UNIQUE FK → `users` (RESTRICT). `balance_paise INTEGER CHECK (>= 0)`. |
| `transactions` | Deposits and transfers (append-only) | UNIQUE `reference`. `type` ∈ {DEPOSIT, TRANSFER}. `status` ∈ {SUCCESS, FAILED}. `from_wallet_id`/`to_wallet_id` FK → `wallets`. `amount_paise > 0`. `failure_reason` present exactly when FAILED. Deposits have no source; a transfer can't go to its own source. `payee_upi_id`. Indexes on `(from_wallet_id, id)` and `(to_wallet_id, id)`. No UPDATE or DELETE (triggers). |
| `login_attempts` | Lockout and audit | `identifier` (case-insensitive; a digest for unknown accounts). Nullable `user_id` FK (CASCADE). `succeeded` ∈ {0,1}. Indexes on `(identifier, attempted_at)` and `(user_id, attempted_at)`. |
| `idempotency_keys` | Duplicate-request protection | UNIQUE `(user_id, idempotency_key)`. UNIQUE `transaction_id` FK → `transactions` (RESTRICT). `request_fingerprint`. Index on `created_at`. No UPDATE or DELETE (triggers). |
| `upi_pins` | One PIN hash per user | `user_id` PK FK → `users` (CASCADE). Argon2id `pin_hash`. |
| `upi_pin_attempts` | PIN lockout | `user_id` FK (CASCADE). `succeeded` ∈ {0,1}. Index on `(user_id, attempted_at)`. |

**Migrations** (`src/database/SchemaMigrator.cpp`) are numbered and
append-only. `PRAGMA user_version` records the current version, and opening
an older database upgrades it in place, in a single transaction, keeping the
data:

| Version | Change |
|---|---|
| 1 | `users`, `wallets`, `transactions` |
| 2 | `login_attempts`, `idempotency_keys` |
| 3 | `users.email`. It is nullable for older rows; triggers require it for new ones. |
| 4 | `upi_pins`, `upi_pin_attempts` |
| 5 | Append-only triggers on `transactions`; no updates to idempotency keys |
| 6 | `transactions.payee_upi_id`; idempotency keys can't be deleted |

## Testing and results

```powershell
ctest --test-dir build --output-on-failure   # both suites
.\build\paysphere_tests.exe                  # one PASS/FAIL line per test
```

CTest registers two suites:

1. **`paysphere_tests`:** 166 unit, integration and end-to-end tests, run by
   a small built-in harness.
   * Tests use in-memory or temporary databases and never touch `data/`.
   * A `ManualClock` covers 15-minute lockouts and 12-hour sessions without
     sleeping.
   * libsodium's minimum Argon2id cost keeps the suite fast. The production
     cost is tested separately in `test_validation.cpp`.
2. **`cli_end_to_end`:** `tests/cli/cli_e2e.cmake` runs the **real
   executable** three times on one database file, with scripted keyboard
   input.
   * It checks what is printed, that data persists across restarts, and
     that access control holds.
   * It also asserts that **no password or PIN ever appears in the output**.
   * Transcripts are written to `build/cli-e2e/`.

| File | Tests | Focus |
|---|---|---|
| `test_money.cpp` | 6 | Parsing, Indian formatting, overflow |
| `test_models.cpp` | 7 | Model invariants, the error hierarchy |
| `test_database.cpp` | 11 | Connection, statements, transactions, migrations |
| `test_schema.cpp` | 18 | Every table's constraints, foreign keys, indexes, append-only triggers |
| `test_database_manager.cpp` | 14 | Lifecycle, upgrades, query helpers, SQL injection, commit/rollback |
| `test_validation.cpp` | 10 | Input rules, timestamps, Argon2id |
| `auth_tests.cpp` | 22 | Registration, login, lockout, sessions |
| `wallet_tests.cpp` | 16 | Wallet rules, deposits, atomicity, persistence |
| `payment_tests.cpp` | 28 | UPI payments, PIN, idempotency, rollback, risk rules |
| `transaction_tests.cpp` | 16 | History, search, receipts, access control, immutability, restart |
| `e2e_tests.cpp` | 6 | Complete workflows through `ApplicationContext`, including restarts |
| `qa_tests.cpp` | 12 | Phase 8 audit regressions and database-failure scenarios |
| **Total** | **166** | |

### Results (measured, Phase 8)

These were run on 2026-10-14, on Windows 11 with GCC 16.2.0, CMake 4.4.3 and
Ninja 1.13.2. Each configuration started from an **empty build directory**.

| Configuration | Build | Warnings (`-Werror` on) | `paysphere_tests` | `cli_end_to_end` | CTest total time |
|---|---|---|---|---|---|
| Debug | succeeded in 13 s | 0 | **166/166 passed** | **passed** | 5.3 s |
| Release | succeeded in 60 s | 0 | **166/166 passed** | **passed** | 3.4 s |

**Not executed:** builds and tests on Linux, macOS or MSVC; sanitizers
(ASan/UBSan); fuzzing; tests with real OS threads.

### Requested scenarios and where they are tested

| Scenario | Tests (all passed in the runs above) |
|---|---|
| Duplicate registration | `register_rejects_duplicate_email_upi_id_and_phone`, `e2e_failures_leave_the_system_consistent`, CLI run 1 |
| Invalid login | `login_rejects_incorrect_password_and_records_it`, `login_failure_message_does_not_reveal_if_account_exists`, `lockout_*` (5 tests), `qa_login_lockout_holds_against_parallel_requests`, `qa_unknown_login_identifiers_are_not_stored_in_clear`, CLI run 1 |
| Unauthorized operations | `wallet_access_requires_a_live_session`, `users_cannot_access_other_users_wallets`, `payments_require_a_live_session`, `history_shows_only_the_callers_own_records`, `search_hides_other_users_records_like_unknown_ids`, `receipts_are_refused_to_non_parties`, `e2e_authorization_holds_across_services`, CLI run 3 |
| Invalid amounts | `money_rejects_malformed_amounts`, `deposit_rejects_invalid_amounts_without_recording`, `invalid_amounts_are_rejected_without_a_record`, CLI run 1 (`abc`) |
| Insufficient funds | `insufficient_balance_is_recorded_as_failed_and_moves_nothing`, `money_is_conserved_across_many_transfers`, CLI run 1 |
| Duplicate requests | `duplicate_request_is_executed_only_once`, `duplicate_of_a_failed_request_returns_the_same_failure`, `reusing_a_key_for_a_different_payment_is_rejected`, `idempotency_keys_are_scoped_to_each_user`, `qa_retried_deposit_is_credited_only_once`, `e2e_everything_but_sessions_survives_a_restart` (replay after a restart) |
| Database failures | `qa_database_locked_by_another_process_fails_cleanly`, `qa_unusable_database_location_is_reported`, `qa_corrupt_stored_rows_are_reported_as_corruption`, `qa_database_layer_reports_precise_errors`, `manager_stays_closed_when_initialization_fails`, `migrator_refuses_newer_schema_and_unknown_targets` |
| Rollback | `db_transaction_rolls_back_unless_committed`, `manager_transaction_rolls_back_every_write_on_failure`, `register_is_atomic_when_wallet_creation_fails`, `deposit_is_atomic_when_the_audit_record_cannot_be_written`, `failure_after_the_debit_rolls_back_every_change`, `failure_recording_the_transaction_rolls_back_the_transfer`, `failure_storing_the_idempotency_key_rolls_back_the_transfer` |
| Restart persistence | `e2e_everything_but_sessions_survives_a_restart`, `e2e_security_state_survives_a_restart`, `deposits_persist_across_restarts`, `history_and_receipts_survive_an_application_restart`, `manager_close_and_reopen_keeps_file_data`, `qa_users_created_before_email_existed_can_still_log_in`, CLI runs 2–3 |

### Do the tests catch real defects?

From Phase 2 on, each phase was checked by **deliberately breaking
protections**: removing an ownership check, the balance check, the
compare-and-set, a trigger, the PIN check and so on. The tests were run to
confirm that the right test fails each time. Nine of the Phase 8 fixes were
checked this way, by reverting each fix and confirming that its regression
test fails.

These checks found real test gaps, which were then fixed. The clearest
example came in Phase 6: disabling the delete trigger on `transactions`
broke no test. Every delete in the tests targeted a transfer, whose
idempotency key's foreign key blocked the delete anyway. Tests that delete
a deposit, which has no key, now cover the trigger on its own.

## Phase 8 QA audit

**Method:**
* a clean-directory build and full test run as a baseline: 153/153 unit
  tests and the CLI test passed;
* two independent read-only code reviews by separate AI review agents, one
  for security and correctness and one for database consistency and OOP
  design;
* a review by the main AI assistant;
* every finding checked against the code before acting on it;
* a regression test for each fix, confirmed to fail when the fix is reverted.

### Defects found and fixed

| # | Severity | Finding | Fix | Regression test |
|---|---|---|---|---|
| 1 | Medium | The **UPI PIN attempt limit could be exceeded by parallel requests**. The block was checked before the slow PIN verification, and not again under the write lock. | The block is re-checked inside the payment transaction, without revealing whether the PIN was right | `qa_pin_attempt_limit_holds_against_parallel_requests` |
| 2 | Medium | The **login lockout had the same race**. It also **linked emails to UPI IDs**: lockouts were counted per account, so 4 failures as the UPI ID plus 1 per candidate email locked only the real email. | Lockouts are counted per typed identifier and re-checked atomically with the recorded attempt | `qa_login_lockout_holds_against_parallel_requests`, `lockout_is_per_identifier_so_it_cannot_link_email_and_upi_id` |
| 3 | Medium | A **retried deposit was credited twice**, because deposits had no idempotency key | A shared `IdempotencyLedger` is used by both deposits and payments | `qa_retried_deposit_is_credited_only_once` |
| 4 | Medium | **Users created before schema v3 (no email) could not log in** after an upgrade. Migration 3 allowed NULL emails, but the model rejected them. | The model allows an empty email for those older accounts | `qa_users_created_before_email_existed_can_still_log_in` |
| 5 | Low | A raw unknown login identifier was stored in clear, for example a password typed into the email field | Unknown identifiers are stored as `unknown:<BLAKE2b>` | `qa_unknown_login_identifiers_are_not_stored_in_clear` |
| 6 | Low | **Unlimited UPI ID probing.** Failed transfers were never rate-limited. | At most 10 failed attempts per 10 minutes | `qa_failed_attempts_are_throttled_and_keep_the_attempted_payee` |
| 7 | Low | The **new-payee cap could be bypassed** by a ₹1 first payment | The cap lasts 24 hours after the first payment to that payee | `risk_engine_new_payee_cap_is_not_lifted_by_a_token_payment` |
| 8 | Low | Failed transfers to unknown recipients **lost the intended payee**. Idempotency keys could be deleted and re-pointed. | Migration 6 adds `payee_upi_id` and a delete trigger | `qa_failed_attempts_…`, `qa_database_layer_reports_precise_errors` |
| 9 | Low | `insert()` could return an unrelated row ID when nothing was inserted. Compare-and-set failures were reported as lock errors, and missing records with result code 0. Corrupt rows surfaced as `ValidationError`. | `insert()` requires exactly one row. Added `ConcurrentModificationError` and `CorruptDataError`. | `qa_database_layer_reports_precise_errors`, `qa_corrupt_stored_rows_are_reported_as_corruption` |
| 10 | Low | PINs and passwords were not wiped from memory, and a PIN leaked when the constructor threw | `SecretString`, `secureWipe` and CLI wiping | `qa_secret_strings_take_over_and_wipe_their_source` |
| 11 | Low | The first unknown-account login paid for an extra hash, a one-off timing tell | The dummy hash is computed in the constructor | covered by `login_failure_message_does_not_reveal_if_account_exists` (timing itself not measured) |
| 12 | Low | `audit()` took the write lock. Users and wallets used the database clock while transactions used the app clock. | `inReadTransaction`; timestamps come from the injected clock | `qa_all_timestamps_come_from_the_application_clock` |

### Design and maintainability changes

* **Database types no longer leak into the service API.** `Party` and
  `TransactionDetails` moved to `models/`.
* **`WalletLimits` has its own header**, and the payment limits now derive
  the recipient cap from it, so the two can't drift apart.
* **Duplicated code was merged:** `FailureSummary`, the hex and BLAKE2b
  helpers, and the "wallet must exist" lookup (`requireByUserId`).
* **The SQLite busy timeout is configurable**, so tests of lock contention
  run in milliseconds.

## Known limitations

These are documented and deliberately not fixed.

* **One connection per thread.** A `DatabaseManager` is one SQLite
  connection and must not be shared between threads. Concurrency between
  processes is safe: `BEGIN IMMEDIATE` and the busy timeout serialize writes.
  The tests simulate parallel requests deterministically with test doubles;
  real OS threads were not tested.
* **Sessions live in memory**, so they end when the program exits. A server
  would store token hashes in a table.
* **Setting a PIN doesn't require re-entering the password.** A stolen
  session on an account without a PIN could set one. A real app would ask
  for the password again.
* **PINs can't be changed or reset once set.**
* **PIN hashes alone don't resist offline cracking.** With 10⁴–10⁶ possible
  PINs, a stolen database could be brute-forced. Real systems add a secret
  pepper held in an HSM.
* **Lockouts can be used for denial of service.** Anyone who knows an
  identifier can keep it locked. Real systems add CAPTCHAs or device
  recognition.
* **Registration reveals which email, UPI ID or phone is taken.** That's a
  usability trade-off; login does not reveal it.
* **Direct database access can still rewrite history.** Someone with that
  access could `DROP TRIGGER` and then edit records. A hash chain or an
  external append-only log would detect this.
* **History pagination uses offsets**, so a new transaction arriving between
  page requests can shift entries.
* **The default database path is fixed at build time.** Use `--db` elsewhere.
* **Limits are simulation choices.** The ₹2,00,000 wallet cap is modelled on
  the RBI's full-KYC prepaid-wallet limit; check the current RBI Master
  Direction before citing it. The other limits (₹1,00,000 per payment and per
  day, the new-payee cap, PIN and login lockouts) are not official NPCI or
  RBI rules.

## Project history

| Phase | Delivered |
|---|---|
| 1 | CMake, layout, bundled SQLite, models, `Money`, RAII database layer, migrations |
| 2 | `DatabaseManager`, `login_attempts` and `idempotency_keys` (schema v2) |
| 3 | `AuthService`, `SessionManager`, Argon2id via libsodium, lockout, atomic user + wallet creation (v3) |
| 4 | `WalletService`: deposits, compare-and-set balances, audit records, reconciliation |
| 5 | `PaymentMethod`/`UPIPayment`, `PaymentService`, `UpiPinService`, `RiskEngine`, idempotency (v4) |
| 6 | `TransactionService`: history, search, receipts, append-only records (v5) |
| 7 | `ApplicationContext` integration, the CLI, end-to-end tests |
| 8 | QA audit: 12 findings fixed with regression tests (v6), this README |

## Third-party components

| Component | Version | License | Location |
|---|---|---|---|
| SQLite | 3.54.0 (amalgamation) | Public domain | `third_party/sqlite/` |
| libsodium | 1.0.22 (MinGW x64 static) | ISC | `third_party/libsodium/` (provenance and signature check in its README) |
