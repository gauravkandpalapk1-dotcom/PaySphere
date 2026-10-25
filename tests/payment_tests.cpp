// PaymentMethod/UPIPayment, UpiPinService, RiskEngine and PaymentService.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/DatabaseManager.h"
#include "database/TransactionRepository.h"
#include "models/Errors.h"
#include "payments/Money.h"
#include "payments/PaymentMethod.h"
#include "payments/UPIPayment.h"
#include "services/AuthService.h"
#include "services/PasswordHasher.h"
#include "services/PaymentService.h"
#include "services/ReferenceGenerator.h"
#include "services/RiskEngine.h"
#include "services/SessionManager.h"
#include "services/UpiPinService.h"
#include "services/WalletService.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>

using paysphere::IdempotencyConflictError;
using paysphere::PaymentBlockedError;
using paysphere::PaySphereError;
using paysphere::SessionError;
using paysphere::ValidationError;
using paysphere::db::DatabaseManager;
using paysphere::db::TransactionRepository;
using paysphere::models::TransactionType;
using paysphere::payments::Money;
using paysphere::payments::PaymentMethod;
using paysphere::payments::UPIPayment;
using paysphere::services::AuthService;
using paysphere::services::PaymentResult;
using paysphere::services::PaymentService;
using paysphere::services::RandomReferenceGenerator;
using paysphere::services::RiskContext;
using paysphere::services::RiskEngine;
using paysphere::services::RiskLimits;
using paysphere::services::RiskRule;
using paysphere::services::SessionManager;
using paysphere::services::SodiumPasswordHasher;
using paysphere::services::UpiPinService;
using paysphere::services::WalletService;
using testsupport::ManualClock;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPassword = "correct horse battery";
constexpr const char* kPin = "1357";

Money rupees(const char* amount) {
    return Money::parse(amount);
}

// Two registered users, asha and ravi, each with UPI PIN 1357 and an empty
// wallet, on an in-memory database with a manual clock.
struct PaymentFixture {
    ManualClock clock;
    DatabaseManager db{":memory:"};
    SodiumPasswordHasher hasher{SodiumPasswordHasher::Cost::minimumForTests()};
    SessionManager sessions{clock};
    AuthService auth{db, sessions, hasher, clock};
    RandomReferenceGenerator references;
    WalletService wallets{db, auth, references, clock};
    UpiPinService pins{db, auth, hasher, clock};
    RiskEngine risk;
    PaymentService payments{db, auth, pins, risk, references, clock};
    std::string asha;
    std::string ravi;

    explicit PaymentFixture(RiskLimits limits = {}) : risk(db, clock, limits) {
        db.initialize();
        asha = signUp("asha", "9876543210");
        ravi = signUp("ravi", "9123456789");
    }

    std::string signUpWithoutPin(const std::string& name, const std::string& phone) {
        auth.registerUser({name + " Test", name + "@example.com", phone, name + "@paysphere", kPassword});
        return auth.login(name + "@example.com", kPassword).sessionToken;
    }
    std::string signUp(const std::string& name, const std::string& phone) {
        const std::string token = signUpWithoutPin(name, phone);
        pins.setPin(token, kPin);
        return token;
    }

    void fund(const std::string& token, const char* amount) {
        CHECK(wallets.deposit(token, rupees(amount)).succeeded());
    }
    PaymentResult pay(const std::string& token, const std::string& payee, const char* amount,
                      const std::string& key, const std::string& pin = kPin) {
        const UPIPayment payment(payee, rupees(amount), pin);
        return payments.pay(token, payment, key);
    }

    // Sessions last at most 12 hours, so tests that jump a day ahead log in again.
    std::string relogin(const std::string& name) {
        return auth.login(name + "@example.com", kPassword).sessionToken;
    }

    Money balance(const std::string& token) { return wallets.balance(token); }
    std::int64_t walletId(const std::string& token) { return wallets.myWallet(token).id(); }
    std::int64_t count(const std::string& sql) { return db.queryInt64(sql).value(); }
    std::int64_t transfers() { return count("SELECT count(*) FROM transactions WHERE type = 'TRANSFER'"); }
    void checkLedgersReconcile() {
        TransactionRepository transactions(db);
        CHECK_EQ(transactions.ledgerBalance(walletId(asha)), balance(asha));
        CHECK_EQ(transactions.ledgerBalance(walletId(ravi)), balance(ravi));
    }
};

} // namespace

// ----------------------------------------------------- PaymentMethod / UPI

TEST(upi_payment_validates_its_details) {
    const UPIPayment payment(" Ravi@PaySphere ", rupees("10"), "1357");
    const PaymentMethod& method = payment; // used through the abstract base
    CHECK_EQ(std::string(method.methodName()), "UPI");
    CHECK_EQ(method.payeeAddress(), "ravi@paysphere");
    CHECK_EQ(method.amount(), rupees("10"));
    UPIPayment sixDigitPin("ravi@paysphere", rupees("10"), "135792");

    CHECK_THROWS_AS(UPIPayment("not-a-upi-id", rupees("10"), "1357"), ValidationError);
    CHECK_THROWS_AS(UPIPayment("ravi@paysphere", Money::zero(), "1357"), ValidationError);
    CHECK_THROWS_AS(UPIPayment("ravi@paysphere", Money::fromPaise(-100), "1357"), ValidationError);
    for (const char* pin : {"", "135", "13579", "1357a", "1234567", " 1357"}) {
        CHECK_THROWS_AS(UPIPayment("ravi@paysphere", rupees("10"), pin), ValidationError);
    }
}

TEST(request_descriptor_identifies_the_request_but_not_the_pin) {
    const UPIPayment a("ravi@paysphere", rupees("10"), "1357");
    const UPIPayment sameRequestOtherPin("RAVI@paysphere", rupees("10"), "2468");
    const UPIPayment otherAmount("ravi@paysphere", rupees("10.01"), "1357");
    const UPIPayment otherPayee("asha@paysphere", rupees("10"), "1357");
    CHECK_EQ(a.requestDescriptor(), sameRequestOtherPin.requestDescriptor());
    CHECK(a.requestDescriptor() != otherAmount.requestDescriptor());
    CHECK(a.requestDescriptor() != otherPayee.requestDescriptor());
    CHECK(a.requestDescriptor().find("1357") == std::string::npos);
}

// ---------------------------------------------------------------- UPI PIN

TEST(upi_pin_is_hashed_set_once_and_not_trivial) {
    PaymentFixture f;
    const std::string meera = f.signUpWithoutPin("meera", "9000000003");
    for (const char* pin : {"1111", "1234", "4321", "0123", "123456", "987654", "000000", "12a4", "123"}) {
        CHECK_THROWS_AS(f.pins.setPin(meera, pin), ValidationError);
    }
    CHECK_THROWS_AS(f.pins.setPin("bad-token", "2580"), SessionError);
    f.pins.setPin(meera, "2580");
    CHECK_THROWS_AS(f.pins.setPin(meera, "1470"), ValidationError); // already set

    const std::string hash = f.db.queryText(
        "SELECT pin_hash FROM upi_pins JOIN users ON users.id = upi_pins.user_id "
        "WHERE users.upi_id = 'meera@paysphere'").value();
    CHECK_EQ(hash.rfind("$argon2id$", 0), 0u);
    CHECK(hash.find("2580") == std::string::npos);
}

TEST(payment_requires_a_upi_pin) {
    PaymentFixture f;
    const std::string meera = f.signUpWithoutPin("meera", "9000000003");
    f.fund(meera, "100");
    CHECK_THROWS_AS(f.pay(meera, "ravi@paysphere", "10", "no-pin-key-1"), ValidationError);
    CHECK_EQ(f.transfers(), 0);
}

// ---------------------------------------------------------------- success

TEST(transfer_debits_credits_and_records_atomically) {
    PaymentFixture f;
    f.fund(f.asha, "1000");

    const PaymentResult result = f.pay(f.asha, "ravi@paysphere", "250.50", "key-success-1");
    const auto& transfer = result.transaction;
    CHECK(!result.replayed);
    CHECK(transfer.succeeded());
    CHECK(transfer.type() == TransactionType::Transfer);
    CHECK_EQ(transfer.fromWalletId().value(), f.walletId(f.asha));
    CHECK_EQ(transfer.toWalletId().value(), f.walletId(f.ravi));
    CHECK_EQ(transfer.amount().paise(), 25050);
    CHECK_EQ(transfer.reference().size(), 23u);
    CHECK_EQ(transfer.reference().rfind("TRF20261014", 0), 0u);
    CHECK_EQ(transfer.createdAt(), "2026-10-14T10:00:00Z");

    CHECK_EQ(f.balance(f.asha), rupees("749.50"));
    CHECK_EQ(f.balance(f.ravi), rupees("250.50"));
    f.checkLedgersReconcile();
    CHECK_EQ(f.db.queryInt64("SELECT transaction_id FROM idempotency_keys WHERE idempotency_key = ?",
                             {"key-success-1"}).value(),
             transfer.id());
    CHECK_EQ(f.count("SELECT count(*) FROM upi_pin_attempts WHERE succeeded = 1"), 1);
}

TEST(recipient_upi_id_is_matched_case_insensitively) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    CHECK(f.pay(f.asha, " RAVI@PaySphere ", "10", "key-case-001").transaction.succeeded());
    CHECK_EQ(f.balance(f.ravi), rupees("10"));
}

TEST(money_is_conserved_across_many_transfers) {
    PaymentFixture f;
    f.fund(f.asha, "5000");
    f.fund(f.ravi, "5000");
    int succeeded = 0;
    int failed = 0;
    for (int i = 0; i < 30; ++i) {
        const bool fromAsha = i % 3 != 0;
        const std::string amount = std::to_string(97 * (i + 1) % 4000 + 1) + ".25";
        const PaymentResult result =
            f.pay(fromAsha ? f.asha : f.ravi, fromAsha ? "ravi@paysphere" : "asha@paysphere",
                  amount.c_str(), "conserve-key-" + std::to_string(i));
        (result.transaction.succeeded() ? succeeded : failed)++;
        CHECK_EQ(f.balance(f.asha) + f.balance(f.ravi), rupees("10000")); // nothing created or lost
        f.clock.advance(3min); // stay under the velocity limit
    }
    CHECK(succeeded > 0);
    CHECK(failed > 0); // some ran out of money, which must not break the totals
    f.checkLedgersReconcile();
}

// ------------------------------------------------------- insufficient funds

TEST(insufficient_balance_is_recorded_as_failed_and_moves_nothing) {
    PaymentFixture f;
    f.fund(f.asha, "100");

    const auto failed = f.pay(f.asha, "ravi@paysphere", "100.01", "key-insufficient-1").transaction;
    CHECK(!failed.succeeded());
    CHECK_EQ(failed.failureReason(), PaymentService::kInsufficientFunds);
    CHECK(failed.isPersisted());
    CHECK_EQ(f.balance(f.asha), rupees("100"));
    CHECK(f.balance(f.ravi).isZero());
    f.checkLedgersReconcile();

    // Exactly the whole balance is allowed.
    CHECK(f.pay(f.asha, "ravi@paysphere", "100", "key-insufficient-2").transaction.succeeded());
    CHECK(f.balance(f.asha).isZero());
    CHECK_EQ(f.balance(f.ravi), rupees("100"));
}

// -------------------------------------------------------- invalid recipients

TEST(unknown_recipient_is_recorded_as_failed) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    const auto failed = f.pay(f.asha, "nobody@paysphere", "10", "key-unknown-1").transaction;
    CHECK(!failed.succeeded());
    CHECK_EQ(failed.failureReason(), PaymentService::kRecipientNotFound);
    CHECK(!failed.toWalletId().has_value());
    CHECK_EQ(f.balance(f.asha), rupees("100"));
    CHECK_EQ(f.count("SELECT count(*) FROM upi_pin_attempts"), 0); // PIN never consulted
}

TEST(malformed_or_own_upi_id_is_rejected_without_a_record) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    CHECK_THROWS_AS(f.pay(f.asha, "ravi", "10", "key-malformed-1"), ValidationError);
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@pay.sphere", "10", "key-malformed-2"), ValidationError);
    CHECK_THROWS_AS(f.pay(f.asha, "ASHA@paysphere", "10", "key-self-0001"), ValidationError);
    CHECK_EQ(f.transfers(), 0);
    CHECK_EQ(f.balance(f.asha), rupees("100"));
}

// ---------------------------------------------------- amounts and sessions

TEST(invalid_amounts_are_rejected_without_a_record) {
    PaymentFixture f;
    f.fund(f.asha, "100000");
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "0", "key-amount-01"), ValidationError);
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "0.99", "key-amount-02"), ValidationError);
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "100000.01", "key-amount-03"), ValidationError);
    CHECK_EQ(f.transfers(), 0);
}

TEST(payments_require_a_live_session) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    CHECK_THROWS_AS(f.pay("forged-token", "ravi@paysphere", "10", "key-session-1"), SessionError);
    f.clock.advance(31min);
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "10", "key-session-2"), SessionError);
    const std::string fresh = f.auth.login("asha@example.com", kPassword).sessionToken;
    f.auth.logout(fresh);
    CHECK_THROWS_AS(f.pay(fresh, "ravi@paysphere", "10", "key-session-3"), SessionError);
    CHECK_EQ(f.transfers(), 0);
}

// ---------------------------------------------------------------- the PIN

TEST(incorrect_pin_fails_and_three_strikes_block_payments) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    for (int i = 1; i <= 3; ++i) {
        const auto failed = f.pay(f.asha, "ravi@paysphere", "10", "key-wrong-pin-" + std::to_string(i), "2468");
        CHECK_EQ(failed.transaction.failureReason(), PaymentService::kIncorrectPin);
    }
    CHECK_EQ(f.balance(f.asha), rupees("1000"));

    try {
        f.pay(f.asha, "ravi@paysphere", "10", "key-blocked-01"); // even with the correct PIN
        CHECK(false);
    } catch (const PaymentBlockedError& error) {
        CHECK_EQ(error.blockedUntil(), "2026-10-15T10:00:00Z");
    }
    CHECK_EQ(f.transfers(), 3); // the blocked attempt left no record

    f.clock.advance(24h);
    f.asha = f.relogin("asha");
    CHECK(f.pay(f.asha, "ravi@paysphere", "10", "key-unblocked1").transaction.succeeded());
}

TEST(correct_pin_resets_the_strike_count) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    f.pay(f.asha, "ravi@paysphere", "10", "key-strike-01", "2468");
    f.pay(f.asha, "ravi@paysphere", "10", "key-strike-02", "2468");
    CHECK(f.pay(f.asha, "ravi@paysphere", "10", "key-strike-03").transaction.succeeded());
    f.pay(f.asha, "ravi@paysphere", "10", "key-strike-04", "2468");
    f.pay(f.asha, "ravi@paysphere", "10", "key-strike-05", "2468");
    CHECK(f.pay(f.asha, "ravi@paysphere", "10", "key-strike-06").transaction.succeeded());
}

// ------------------------------------------------------------- duplicates

TEST(duplicate_request_is_executed_only_once) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    const PaymentResult first = f.pay(f.asha, "ravi@paysphere", "100", "dup-key-0001");
    const PaymentResult retry = f.pay(f.asha, "ravi@paysphere", "100", "dup-key-0001");

    CHECK(!first.replayed);
    CHECK(retry.replayed);
    CHECK_EQ(retry.transaction.id(), first.transaction.id());
    CHECK_EQ(retry.transaction.reference(), first.transaction.reference());
    CHECK_EQ(f.balance(f.asha), rupees("900")); // debited once
    CHECK_EQ(f.balance(f.ravi), rupees("100"));
    CHECK_EQ(f.transfers(), 1);

    // A replay returns the stored result without re-running anything, so
    // even a mistyped PIN on the retry neither fails nor counts as a strike.
    CHECK(f.pay(f.asha, "ravi@paysphere", "100", "dup-key-0001", "2468").replayed);
    CHECK_EQ(f.count("SELECT count(*) FROM upi_pin_attempts WHERE succeeded = 0"), 0);
}

TEST(duplicate_of_a_failed_request_returns_the_same_failure) {
    PaymentFixture f;
    f.fund(f.asha, "50");
    const auto failed = f.pay(f.asha, "ravi@paysphere", "100", "dup-key-fail1").transaction;
    CHECK_EQ(failed.failureReason(), PaymentService::kInsufficientFunds);

    f.fund(f.asha, "100");
    const PaymentResult retry = f.pay(f.asha, "ravi@paysphere", "100", "dup-key-fail1");
    CHECK(retry.replayed);
    CHECK_EQ(retry.transaction.id(), failed.id()); // still the original failure
    CHECK_EQ(f.balance(f.asha), rupees("150"));

    CHECK(f.pay(f.asha, "ravi@paysphere", "100", "dup-key-fail2").transaction.succeeded()); // new key
    CHECK_EQ(f.balance(f.asha), rupees("50"));
}

TEST(reusing_a_key_for_a_different_payment_is_rejected) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    f.pay(f.asha, "ravi@paysphere", "100", "reused-key-01");
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "200", "reused-key-01"), IdempotencyConflictError);
    CHECK_THROWS_AS(f.pay(f.asha, "nobody@paysphere", "100", "reused-key-01"), IdempotencyConflictError);
    CHECK_EQ(f.transfers(), 1);
    CHECK_EQ(f.balance(f.asha), rupees("900"));
}

TEST(idempotency_keys_are_scoped_to_each_user) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    f.fund(f.ravi, "100");
    CHECK(!f.pay(f.asha, "ravi@paysphere", "10", "shared-key-01").replayed);
    CHECK(!f.pay(f.ravi, "asha@paysphere", "30", "shared-key-01").replayed);
    CHECK_EQ(f.transfers(), 2);
    CHECK_EQ(f.balance(f.asha), rupees("120"));
}

TEST(idempotency_key_format_is_validated) {
    PaymentFixture f;
    f.fund(f.asha, "100");
    for (const std::string& key : {std::string(), std::string("short"), std::string(129, 'k'),
                                   std::string("has space 1"), std::string("semi;colon1")}) {
        CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "10", key), ValidationError);
    }
    std::set<std::string> generated;
    for (int i = 0; i < 50; ++i) {
        generated.insert(PaymentService::newIdempotencyKey());
    }
    CHECK_EQ(generated.size(), 50u);
    CHECK(f.pay(f.asha, "ravi@paysphere", "10", *generated.begin()).transaction.succeeded());
}

// --------------------------------------------------------------- rollback

TEST(failure_after_the_debit_rolls_back_every_change) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    // The sender's debit succeeds; the receiver's credit then fails.
    f.db.connection().execute("CREATE TRIGGER fail_credit BEFORE UPDATE ON wallets "
                              "WHEN NEW.balance_paise > OLD.balance_paise "
                              "BEGIN SELECT RAISE(ABORT, 'credit failed'); END");

    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "100", "rollback-key-1"), PaySphereError);
    CHECK_EQ(f.balance(f.asha), rupees("1000")); // the debit was undone
    CHECK(f.balance(f.ravi).isZero());
    CHECK_EQ(f.transfers(), 0);
    // (Deposits store idempotency keys too, so count only the transfer's.)
    CHECK_EQ(f.count("SELECT count(*) FROM idempotency_keys k JOIN transactions t ON t.id = k.transaction_id "
                     "WHERE t.type = 'TRANSFER'"),
             0);
    CHECK_EQ(f.count("SELECT count(*) FROM upi_pin_attempts"), 0);

    // Nothing was stored, so the client can safely retry with the same key.
    f.db.connection().execute("DROP TRIGGER fail_credit");
    const PaymentResult retry = f.pay(f.asha, "ravi@paysphere", "100", "rollback-key-1");
    CHECK(!retry.replayed);
    CHECK(retry.transaction.succeeded());
    CHECK_EQ(f.balance(f.asha), rupees("900"));
}

TEST(failure_recording_the_transaction_rolls_back_the_transfer) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    f.db.connection().execute("CREATE TRIGGER no_audit BEFORE INSERT ON transactions "
                              "WHEN NEW.type = 'TRANSFER' "
                              "BEGIN SELECT RAISE(ABORT, 'audit log unavailable'); END");
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "100", "rollback-key-2"), PaySphereError);
    CHECK_EQ(f.balance(f.asha), rupees("1000"));
    CHECK(f.balance(f.ravi).isZero());
}

TEST(failure_storing_the_idempotency_key_rolls_back_the_transfer) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    f.db.connection().execute("CREATE TRIGGER no_keys BEFORE INSERT ON idempotency_keys "
                              "BEGIN SELECT RAISE(ABORT, 'key store unavailable'); END");
    CHECK_THROWS_AS(f.pay(f.asha, "ravi@paysphere", "100", "rollback-key-3"), PaySphereError);
    CHECK_EQ(f.balance(f.asha), rupees("1000"));
    CHECK(f.balance(f.ravi).isZero());
    CHECK_EQ(f.transfers(), 0);
}

// ------------------------------------------------------------------- risk

TEST(risk_engine_limits_payment_velocity) {
    PaymentFixture f;
    f.fund(f.asha, "1000");
    for (int i = 0; i < 5; ++i) {
        CHECK(f.pay(f.asha, "ravi@paysphere", "1", "velocity-key-" + std::to_string(i)).transaction.succeeded());
    }
    const auto declined = f.pay(f.asha, "ravi@paysphere", "1", "velocity-key-5").transaction;
    CHECK(!declined.succeeded());
    CHECK_EQ(declined.failureReason().rfind(PaymentService::kRiskDeclinedPrefix, 0), 0u);
    CHECK_EQ(f.balance(f.asha), rupees("995"));

    f.clock.advance(10min);
    CHECK(f.pay(f.asha, "ravi@paysphere", "1", "velocity-key-6").transaction.succeeded());
}

TEST(risk_engine_enforces_a_rolling_daily_limit) {
    PaymentFixture f(RiskLimits{100, 10min, rupees("1000"), rupees("1000")});
    f.fund(f.asha, "5000");
    CHECK(f.pay(f.asha, "ravi@paysphere", "600", "daily-key-001").transaction.succeeded());
    CHECK(f.pay(f.asha, "ravi@paysphere", "400", "daily-key-002").transaction.succeeded()); // exactly the limit
    const auto declined = f.pay(f.asha, "ravi@paysphere", "1", "daily-key-003").transaction;
    CHECK(declined.failureReason().find("24-hour limit") != std::string::npos);

    f.clock.advance(24h);
    f.asha = f.relogin("asha");
    CHECK(f.pay(f.asha, "ravi@paysphere", "1", "daily-key-004").transaction.succeeded());
}

TEST(risk_engine_caps_the_first_payment_to_a_new_payee) {
    PaymentFixture f;
    f.fund(f.asha, "50000");
    const auto declined = f.pay(f.asha, "ravi@paysphere", "20000.01", "new-payee-01").transaction;
    CHECK(declined.failureReason().find("new recipient") != std::string::npos);
    CHECK(f.pay(f.asha, "ravi@paysphere", "20000", "new-payee-02").transaction.succeeded());
    // Audit fix: a first payment no longer lifts the cap at once (a Rs. 1
    // "verification" payment used to); it lasts 24 hours after the first one.
    CHECK(!f.pay(f.asha, "ravi@paysphere", "25000", "new-payee-03").transaction.succeeded());
    f.clock.advance(24h);
    f.asha = f.relogin("asha");
    CHECK(f.pay(f.asha, "ravi@paysphere", "25000", "new-payee-04").transaction.succeeded());
    CHECK_EQ(f.balance(f.asha), rupees("5000"));
}

TEST(risk_engine_new_payee_cap_is_not_lifted_by_a_token_payment) {
    PaymentFixture f;
    f.fund(f.asha, "50000");
    CHECK(f.pay(f.asha, "ravi@paysphere", "1", "token-pay-001").transaction.succeeded());
    const auto declined = f.pay(f.asha, "ravi@paysphere", "30000", "token-pay-002").transaction;
    CHECK_EQ(declined.failureReason().rfind(PaymentService::kRiskDeclinedPrefix, 0), 0u);
    CHECK_EQ(f.balance(f.asha), rupees("49999"));
}

TEST(risk_engine_accepts_custom_rules) {
    class NoThirteenPaise final : public RiskRule {
    public:
        const char* name() const noexcept override { return "no-13-paise"; }
        std::optional<std::string> check(const RiskContext& context) const override {
            if (context.amount.paise() % 100 == 13) {
                return std::string("Amounts ending in .13 are not accepted");
            }
            return std::nullopt;
        }
    };
    PaymentFixture f;
    f.risk.addRule(std::make_unique<NoThirteenPaise>());
    f.fund(f.asha, "100");
    const auto declined = f.pay(f.asha, "ravi@paysphere", "10.13", "custom-rule-1").transaction;
    CHECK(declined.failureReason().find("ending in .13") != std::string::npos);
    CHECK(f.pay(f.asha, "ravi@paysphere", "10.12", "custom-rule-2").transaction.succeeded());
}

// ------------------------------------------------------- receiver limits

TEST(recipient_wallet_cap_is_enforced) {
    PaymentFixture f;
    f.fund(f.ravi, "100000");
    f.fund(f.ravi, "100000"); // ravi is at the Rs. 2,00,000 cap
    f.fund(f.asha, "100");
    const auto failed = f.pay(f.asha, "ravi@paysphere", "1", "cap-key-0001").transaction;
    CHECK_EQ(failed.failureReason(), PaymentService::kRecipientLimit);
    CHECK_EQ(f.balance(f.asha), rupees("100"));
    CHECK_EQ(f.balance(f.ravi), rupees("200000"));
}
