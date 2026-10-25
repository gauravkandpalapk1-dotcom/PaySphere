// Phase 8 QA audit: one regression test per defect found in the audit, plus
// database-failure scenarios (lock contention, unwritable location, corrupt
// rows) that the other suites did not cover.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/Database.h"
#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "database/Statement.h"
#include "database/WalletRepository.h"
#include "models/Errors.h"
#include "payments/Money.h"
#include "payments/PaymentMethod.h"
#include "payments/SecretString.h"
#include "payments/UPIPayment.h"
#include "services/ApplicationContext.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

using paysphere::AccountLockedError;
using paysphere::AuthenticationError;
using paysphere::IdempotencyConflictError;
using paysphere::PaymentBlockedError;
using paysphere::db::ConcurrentModificationError;
using paysphere::db::ConstraintViolation;
using paysphere::db::CorruptDataError;
using paysphere::db::Database;
using paysphere::db::DatabaseError;
using paysphere::db::SchemaMigrator;
using paysphere::db::Statement;
using paysphere::db::WalletRepository;
using paysphere::payments::CredentialVerifier;
using paysphere::payments::Money;
using paysphere::payments::PaymentMethod;
using paysphere::payments::SecretString;
using paysphere::payments::UPIPayment;
using paysphere::services::ApplicationContext;
using paysphere::services::ApplicationOptions;
using paysphere::services::AuthService;
using paysphere::services::PasswordHasher;
using paysphere::services::PaymentService;
using paysphere::services::SessionManager;
using paysphere::services::SodiumPasswordHasher;
using testsupport::ManualClock;
using testsupport::TempDirectory;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPassword = "correct horse battery";
constexpr const char* kPin = "1357";

Money rupees(const char* amount) {
    return Money::parse(amount);
}

// The real composition root on a database file, with a short lock timeout so
// lock-contention tests do not wait 5 seconds.
struct Installation {
    TempDirectory directory;
    ManualClock clock;
    std::string path = directory.file("paysphere.db");
    std::unique_ptr<ApplicationContext> app;

    Installation() { start(); }
    void start() {
        app = std::make_unique<ApplicationContext>(
            path, ApplicationOptions{SodiumPasswordHasher::Cost::minimumForTests(), &clock, 50ms});
    }
    std::string onboard(const std::string& name, const std::string& phone) {
        std::string fullName = name + " Test";
        fullName[0] = static_cast<char>(fullName[0] - 'a' + 'A');
        app->auth().registerUser({fullName, name + "@example.com", phone, name + "@paysphere", kPassword});
        const std::string token = app->auth().login(name + "@example.com", kPassword).sessionToken;
        app->pins().setPin(token, kPin);
        return token;
    }
    paysphere::services::PaymentResult pay(const std::string& token, const std::string& payee, const char* amount,
                                           const std::string& pin = kPin) {
        const UPIPayment payment(payee, rupees(amount), pin);
        return app->payments().pay(token, payment, PaymentService::newIdempotencyKey());
    }
    std::int64_t count(const std::string& sql) { return app->database().queryInt64(sql).value(); }
};

// A UPI payment whose PIN check is slow enough for parallel requests to slip
// in: while it is "verifying", `meanwhile` runs (it records what concurrent
// requests would have recorded), then the real PIN check happens.
class RacingPayment final : public PaymentMethod {
public:
    RacingPayment(std::string payee, Money amount, std::function<void()> meanwhile)
        : PaymentMethod(amount), payee_(std::move(payee)), meanwhile_(std::move(meanwhile)) {}

    const char* methodName() const noexcept override { return "UPI"; }
    const std::string& payeeAddress() const noexcept override { return payee_; }
    bool authorize(std::int64_t payerUserId, CredentialVerifier& verifier) const override {
        meanwhile_();
        return verifier.verifyCredential(payerUserId, kPin); // the correct PIN
    }

private:
    std::string payee_;
    std::function<void()> meanwhile_;
};

// A password hasher whose verify() is slow enough for parallel logins to
// slip in, in the same way.
class RacingHasher final : public PasswordHasher {
public:
    RacingHasher(const PasswordHasher& real, std::function<void()> meanwhile)
        : real_(real), meanwhile_(std::move(meanwhile)) {}

    std::string hash(const std::string& password) const override { return real_.hash(password); }
    bool verify(const std::string& encodedHash, const std::string& password) const override {
        if (armed) {
            meanwhile_();
        }
        return real_.verify(encodedHash, password);
    }
    bool needsRehash(const std::string& encodedHash) const override { return real_.needsRehash(encodedHash); }

    bool armed = false;

private:
    const PasswordHasher& real_;
    std::function<void()> meanwhile_;
};

} // namespace

// --------------------------------------------- audit finding: race windows

TEST(qa_pin_attempt_limit_holds_against_parallel_requests) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    i.onboard("ravi", "9123456789");
    i.app->wallets().deposit(asha, rupees("1000"));
    const std::int64_t ashaId = i.app->auth().authenticate(asha).id();

    // While this payment verifies its (correct) PIN, three parallel requests
    // with wrong PINs complete and use up the PIN attempts.
    const RacingPayment payment("ravi@paysphere", rupees("100"), [&] {
        for (int n = 0; n < 3; ++n) {
            i.app->pins().recordAttempt(ashaId, false);
        }
    });
    CHECK_THROWS_AS(i.app->payments().pay(asha, payment, "race-key-0001"), PaymentBlockedError);
    // Before the fix this payment went through, giving a 4th PIN guess.
    CHECK_EQ(i.app->wallets().balance(asha), rupees("1000"));
    CHECK_EQ(i.count("SELECT count(*) FROM transactions WHERE type = 'TRANSFER'"), 0);
}

TEST(qa_login_lockout_holds_against_parallel_requests) {
    ManualClock clock;
    paysphere::db::DatabaseManager db(":memory:");
    db.initialize();
    const SodiumPasswordHasher realHasher(SodiumPasswordHasher::Cost::minimumForTests());
    paysphere::db::LoginAttemptRepository attempts(db);
    RacingHasher hasher(realHasher, [&] {
        for (int n = 0; n < 5; ++n) { // five parallel wrong guesses finish first
            attempts.record("asha@example.com", 1, false, paysphere::services::formatTimestamp(clock.now()));
        }
    });
    SessionManager sessions(clock);
    AuthService auth(db, sessions, hasher, clock);
    auth.registerUser({"Asha Test", "asha@example.com", "9876543210", "asha@paysphere", kPassword});

    hasher.armed = true;
    // Even the correct password must not get in (nor reveal that it was right).
    CHECK_THROWS_AS(auth.login("asha@example.com", kPassword), AccountLockedError);
    CHECK_EQ(sessions.activeCount(), 0u);
    CHECK_EQ(db.queryInt64("SELECT count(*) FROM login_attempts WHERE succeeded = 1").value(), 0);
}

// ------------------------------------------ audit finding: privacy of logs

TEST(qa_unknown_login_identifiers_are_not_stored_in_clear) {
    Installation i;
    // Someone types their password into the email field.
    CHECK_THROWS_AS(i.app->auth().login("my secret password", "whatever"), AuthenticationError);
    CHECK_EQ(i.count("SELECT count(*) FROM login_attempts WHERE identifier LIKE '%secret%'"), 0);
    CHECK_EQ(i.count("SELECT count(*) FROM login_attempts WHERE identifier LIKE 'unknown:%'"), 1);
    // ...and lockout still works for such identifiers.
    for (int n = 0; n < 4; ++n) {
        CHECK_THROWS_AS(i.app->auth().login("my secret password", "whatever"), AuthenticationError);
    }
    CHECK_THROWS_AS(i.app->auth().login("my secret password", "whatever"), AccountLockedError);
}

// --------------------------------- audit finding: duplicate deposit request

TEST(qa_retried_deposit_is_credited_only_once) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    const auto first = i.app->wallets().deposit(asha, rupees("100"), "deposit-key-0001");
    const auto retry = i.app->wallets().deposit(asha, rupees("100"), "deposit-key-0001");
    CHECK_EQ(retry.id(), first.id());
    CHECK_EQ(i.app->wallets().balance(asha), rupees("100")); // was Rs. 200 before the fix

    CHECK_THROWS_AS(i.app->wallets().deposit(asha, rupees("250"), "deposit-key-0001"), IdempotencyConflictError);
    // Keys are shared across request types for a user, so a deposit key
    // cannot be reused for a payment either.
    i.onboard("ravi", "9123456789");
    const UPIPayment payment("ravi@paysphere", rupees("10"), kPin);
    CHECK_THROWS_AS(i.app->payments().pay(asha, payment, "deposit-key-0001"), IdempotencyConflictError);
    CHECK(i.app->transactions().audit(asha).consistent());
}

// ------------------------------------ audit finding: legacy (v2) databases

TEST(qa_users_created_before_email_existed_can_still_log_in) {
    TempDirectory directory;
    const std::string path = directory.file("legacy.db");
    {
        Database legacy(path);
        SchemaMigrator migrator(legacy);
        migrator.migrate(2); // a database from before email was added
        const SodiumPasswordHasher hasher(SodiumPasswordHasher::Cost::minimumForTests());
        Statement insert = legacy.prepare(
            "INSERT INTO users (full_name, phone, upi_id, password_hash) VALUES ('Old User', '9000000009', "
            "'old@paysphere', ?)");
        insert.bind(1, hasher.hash(kPassword));
        insert.step();
        legacy.execute("INSERT INTO wallets (user_id, balance_paise) VALUES (1, 5000)");
    }

    ManualClock clock;
    ApplicationContext app(path, ApplicationOptions{SodiumPasswordHasher::Cost::minimumForTests(), &clock});
    CHECK_EQ(app.database().schemaVersion(), SchemaMigrator::kLatestVersion);
    // Before the fix, loading this user threw "User email is required".
    const auto login = app.auth().login("old@paysphere", kPassword);
    CHECK(login.user.email().empty());
    CHECK_EQ(app.wallets().balance(login.sessionToken), rupees("50"));
    CHECK(app.wallets().deposit(login.sessionToken, rupees("10")).succeeded());
}

// --------------------------------- audit finding: probing and the audit log

TEST(qa_failed_attempts_are_throttled_and_keep_the_attempted_payee) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    i.app->wallets().deposit(asha, rupees("100"));
    for (int n = 0; n < 10; ++n) {
        const auto failed = i.pay(asha, "probe" + std::to_string(n) + "@paysphere", "1").transaction;
        CHECK_EQ(failed.failureReason(), PaymentService::kRecipientNotFound);
        CHECK_EQ(failed.payeeUpiId(), "probe" + std::to_string(n) + "@paysphere"); // kept for the audit trail
    }
    // The 11th failed attempt in 10 minutes is refused without a record.
    CHECK_THROWS_AS(i.pay(asha, "probe10@paysphere", "1"), PaymentBlockedError);
    CHECK_EQ(i.count("SELECT count(*) FROM transactions WHERE type = 'TRANSFER'"), 10);

    i.clock.advance(10min);
    CHECK(!i.pay(asha, "probe10@paysphere", "1").transaction.succeeded()); // allowed again
}

// ---------------------------------------------- database failure scenarios

TEST(qa_database_locked_by_another_process_fails_cleanly) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    i.onboard("ravi", "9123456789");
    i.app->wallets().deposit(asha, rupees("1000"));
    {
        // Another process holds the write lock.
        Database other(i.path);
        other.execute("BEGIN IMMEDIATE");
        CHECK_THROWS_AS(i.app->wallets().deposit(asha, rupees("100")), DatabaseError);
        CHECK_THROWS_AS(i.pay(asha, "ravi@paysphere", "100"), DatabaseError);
        // Reads still work while the write lock is held (WAL mode).
        CHECK_EQ(i.app->wallets().balance(asha), rupees("1000"));
        CHECK(i.app->transactions().audit(asha).consistent());
        other.execute("ROLLBACK");
    }
    // Nothing was half-applied, and the same operations succeed afterwards.
    CHECK_EQ(i.count("SELECT count(*) FROM transactions WHERE type = 'TRANSFER'"), 0);
    CHECK(i.pay(asha, "ravi@paysphere", "100").transaction.succeeded());
    CHECK_EQ(i.app->wallets().balance(asha), rupees("900"));
}

TEST(qa_unusable_database_location_is_reported) {
    TempDirectory directory;
    const std::string blocker = directory.file("not-a-directory");
    std::ofstream(std::filesystem::u8path(blocker)) << "x";
    CHECK_THROWS_AS(ApplicationContext(blocker + "/paysphere.db"), DatabaseError);
}

TEST(qa_corrupt_stored_rows_are_reported_as_corruption) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    const std::int64_t walletId = i.app->wallets().myWallet(asha).id();
    // A deposit row with a payee UPI ID passes the schema but breaks the model.
    i.app->database().execute(
        "INSERT INTO transactions (reference, type, status, to_wallet_id, amount_paise, payee_upi_id) "
        "VALUES ('DEPBAD', 'DEPOSIT', 'SUCCESS', ?, 100, 'x@paysphere')",
        {walletId});
    CHECK_THROWS_AS(i.app->transactions().history(asha), CorruptDataError);
    CHECK_THROWS_AS(i.app->transactions().history(asha), DatabaseError); // and it is a database error
}

TEST(qa_database_layer_reports_precise_errors) {
    Installation i;
    const std::string asha = i.onboard("asha", "9876543210");
    auto& db = i.app->database();
    // insert() refuses to return an id when no row was inserted.
    CHECK_THROWS_AS(db.insert("INSERT OR IGNORE INTO wallets (user_id, balance_paise) VALUES (1, 0)"),
                    DatabaseError);
    // A stale compare-and-set has its own error type (not a lock timeout).
    WalletRepository wallets(db);
    const auto wallet = wallets.requireByUserId(1);
    CHECK_THROWS_AS(wallets.updateBalance(wallet.id(), rupees("5"), rupees("6")), ConcurrentModificationError);
    // The idempotency mapping can be neither changed nor deleted.
    i.app->wallets().deposit(asha, rupees("1"), "deposit-key-0002");
    CHECK_THROWS_AS(db.execute("DELETE FROM idempotency_keys"), ConstraintViolation);
    CHECK_EQ(i.count("SELECT count(*) FROM idempotency_keys"), 1);
}

// ------------------------------------------------------- consistency fixes

TEST(qa_all_timestamps_come_from_the_application_clock) {
    Installation i; // manual clock at 2026-10-14T10:00:00Z, far from the real time
    i.onboard("asha", "9876543210");
    CHECK_EQ(i.app->database().queryText("SELECT created_at FROM users").value(), "2026-10-14T10:00:00Z");
    CHECK_EQ(i.app->database().queryText("SELECT created_at FROM wallets").value(), "2026-10-14T10:00:00Z");
}

TEST(qa_secret_strings_take_over_and_wipe_their_source) {
    std::string typed = "1357";
    {
        const SecretString pin(std::move(typed));
        CHECK_EQ(pin.value(), "1357");
    }
    CHECK(typed.empty()); // the moved-from source was wiped and emptied
    std::string password = "correct horse battery staple";
    paysphere::payments::secureWipe(password);
    CHECK(password.empty());
}
