// Wallet model, WalletService and deposit persistence.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/TransactionRepository.h"
#include "database/WalletRepository.h"
#include "models/Errors.h"
#include "models/Wallet.h"
#include "payments/Money.h"
#include "services/AuthService.h"
#include "services/PasswordHasher.h"
#include "services/ReferenceGenerator.h"
#include "services/SessionManager.h"
#include "services/WalletService.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <string>

using paysphere::AuthorizationError;
using paysphere::PaySphereError;
using paysphere::SessionError;
using paysphere::ValidationError;
using paysphere::db::ConstraintViolation;
using paysphere::db::DatabaseError;
using paysphere::db::DatabaseManager;
using paysphere::db::TransactionRepository;
using paysphere::db::WalletRepository;
using paysphere::models::TransactionType;
using paysphere::models::Wallet;
using paysphere::payments::Money;
using paysphere::services::AuthService;
using paysphere::services::RandomReferenceGenerator;
using paysphere::services::ReferenceGenerator;
using paysphere::services::SessionManager;
using paysphere::services::SodiumPasswordHasher;
using paysphere::services::TimePoint;
using paysphere::services::WalletLimits;
using paysphere::services::WalletService;
using testsupport::ManualClock;
using testsupport::TempDirectory;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPassword = "correct horse battery";

Money rupees(const char* amount) {
    return Money::parse(amount);
}

// Hands out scripted references, to force collisions.
class ScriptedReferenceGenerator final : public ReferenceGenerator {
public:
    explicit ScriptedReferenceGenerator(std::deque<std::string> references)
        : references_(std::move(references)) {}

    std::string next(TransactionType, TimePoint) override {
        std::string reference = references_.front();
        references_.pop_front();
        return reference;
    }

private:
    std::deque<std::string> references_;
};

struct WalletFixture {
    ManualClock clock;
    DatabaseManager db;
    SodiumPasswordHasher hasher{SodiumPasswordHasher::Cost::minimumForTests()};
    SessionManager sessions{clock};
    AuthService auth{db, sessions, hasher, clock};
    RandomReferenceGenerator randomReferences;
    WalletService wallets;

    explicit WalletFixture(const std::string& path = ":memory:", ReferenceGenerator* references = nullptr)
        : db(path), wallets(db, auth, references ? *references : randomReferences, clock) {
        db.initialize();
    }

    std::string signUp(const std::string& name, const std::string& phone) {
        auth.registerUser({name + " Test", name + "@example.com", phone, name + "@paysphere", kPassword});
        return login(name);
    }
    std::string login(const std::string& name) {
        return auth.login(name + "@example.com", kPassword).sessionToken;
    }
    std::int64_t count(const std::string& sql) { return db.queryInt64(sql).value(); }
};

bool isUpperHex(const std::string& text) {
    return text.find_first_not_of("0123456789ABCDEF") == std::string::npos;
}

} // namespace

// -------------------------------------------------------------- the model

TEST(wallet_model_encapsulates_balance_rules) {
    const Wallet wallet(1, 7, Money::fromPaise(500));
    CHECK(wallet.belongsTo(7));
    CHECK(!wallet.belongsTo(8));

    CHECK(wallet.canAfford(Money::fromPaise(500)));
    CHECK(!wallet.canAfford(Money::fromPaise(501)));
    CHECK(!wallet.canAfford(Money::zero()));
    CHECK(!wallet.canAfford(Money::fromPaise(-1)));

    CHECK_EQ(wallet.headroom(Money::fromPaise(1000)).paise(), 500);
    CHECK_EQ(wallet.headroom(Money::fromPaise(400)).paise(), 0);
    CHECK(wallet.canReceive(Money::fromPaise(500), Money::fromPaise(1000)));
    CHECK(!wallet.canReceive(Money::fromPaise(501), Money::fromPaise(1000)));
    CHECK(!wallet.canReceive(Money::zero(), Money::fromPaise(1000)));
    // No overflow even for an absurd amount.
    CHECK(!wallet.canReceive(Money::fromPaise(std::numeric_limits<std::int64_t>::max()),
                             Money::fromPaise(1000)));
}

TEST(wallet_limits_are_validated) {
    WalletFixture f;
    RandomReferenceGenerator references;
    CHECK_THROWS_AS(WalletService(f.db, f.auth, references, f.clock, {Money::zero(), rupees("10"), rupees("20")}),
                    ValidationError);
    CHECK_THROWS_AS(WalletService(f.db, f.auth, references, f.clock, {rupees("5"), rupees("1"), rupees("20")}),
                    ValidationError);
    CHECK_THROWS_AS(WalletService(f.db, f.auth, references, f.clock, {rupees("1"), rupees("30"), rupees("20")}),
                    ValidationError);
}

// ---------------------------------------------- retrieval and authorization

TEST(new_wallet_is_empty_and_owned_by_its_user) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    const Wallet wallet = f.wallets.myWallet(token);
    CHECK(wallet.isPersisted());
    CHECK_EQ(wallet.userId(), f.auth.authenticate(token).id());
    CHECK(wallet.balance().isZero());
    CHECK(f.wallets.balance(token).isZero());
}

TEST(wallet_access_requires_a_live_session) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");

    CHECK_THROWS_AS(f.wallets.myWallet(""), SessionError);
    CHECK_THROWS_AS(f.wallets.balance(std::string(64, 'a')), SessionError);
    CHECK_THROWS_AS(f.wallets.deposit("forged", rupees("100")), SessionError);

    f.auth.logout(token);
    CHECK_THROWS_AS(f.wallets.deposit(token, rupees("100")), SessionError);

    const std::string fresh = f.login("asha");
    f.clock.advance(31min); // idle timeout
    CHECK_THROWS_AS(f.wallets.deposit(fresh, rupees("100")), SessionError);
    // An unauthenticated caller gets no feedback on the amount either.
    CHECK_THROWS_AS(f.wallets.deposit(fresh, Money::fromPaise(-5)), SessionError);

    CHECK_EQ(f.count("SELECT count(*) FROM transactions"), 0);
    CHECK_EQ(f.count("SELECT sum(balance_paise) FROM wallets"), 0);
}

TEST(users_cannot_access_other_users_wallets) {
    WalletFixture f;
    const std::string asha = f.signUp("asha", "9876543210");
    const std::string ravi = f.signUp("ravi", "9123456789");
    const Wallet ashaWallet = f.wallets.myWallet(asha);
    const Wallet raviWallet = f.wallets.myWallet(ravi);

    CHECK_EQ(f.wallets.walletById(asha, ashaWallet.id()).id(), ashaWallet.id());
    CHECK_THROWS_AS(f.wallets.walletById(asha, raviWallet.id()), AuthorizationError);
    CHECK_THROWS_AS(f.wallets.walletById(asha, 9999), AuthorizationError); // same error: no probing

    // A deposit only ever reaches the caller's own wallet.
    f.wallets.deposit(asha, rupees("100"));
    CHECK_EQ(f.wallets.balance(asha), rupees("100"));
    CHECK(f.wallets.balance(ravi).isZero());
}

// ---------------------------------------------------------------- deposits

TEST(deposit_credits_balance_and_records_an_audit_entry) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    const std::int64_t walletId = f.wallets.myWallet(token).id();

    const auto deposit = f.wallets.deposit(token, rupees("500.50"));
    CHECK(deposit.isPersisted());
    CHECK(deposit.succeeded());
    CHECK(deposit.type() == TransactionType::Deposit);
    CHECK(!deposit.fromWalletId().has_value());
    CHECK_EQ(deposit.toWalletId().value(), walletId);
    CHECK_EQ(deposit.amount().paise(), 50050);
    CHECK(deposit.failureReason().empty());
    CHECK_EQ(deposit.createdAt(), "2026-10-14T10:00:00Z");
    CHECK_EQ(deposit.reference().size(), 23u);
    CHECK_EQ(deposit.reference().rfind("DEP20261014", 0), 0u);
    CHECK(isUpperHex(deposit.reference().substr(11)));

    CHECK_EQ(f.wallets.balance(token).paise(), 50050);

    // The audit row is in the database exactly as returned, stored as integer paise.
    TransactionRepository transactions(f.db);
    const auto stored = transactions.findByReference(deposit.reference());
    CHECK(stored.has_value());
    CHECK_EQ(stored->id(), deposit.id());
    CHECK_EQ(stored->amount(), deposit.amount());
    CHECK_EQ(f.db.queryText("SELECT typeof(amount_paise) FROM transactions").value(), "integer");
}

TEST(deposits_accumulate_and_reconcile_with_the_ledger) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    for (const char* amount : {"1", "250.75", "99999.99", "0012.30"}) {
        CHECK(f.wallets.deposit(token, rupees(amount)).succeeded());
        f.clock.advance(1min);
    }
    const Money expected = rupees("1") + rupees("250.75") + rupees("99999.99") + rupees("12.30");
    CHECK_EQ(f.wallets.balance(token), expected);

    TransactionRepository transactions(f.db);
    CHECK_EQ(transactions.ledgerBalance(f.wallets.myWallet(token).id()), expected);
    CHECK_EQ(f.count("SELECT count(*) FROM transactions WHERE status = 'SUCCESS'"), 4);
}

TEST(deposit_rejects_invalid_amounts_without_recording) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    const WalletLimits& limits = f.wallets.limits();

    CHECK_THROWS_AS(f.wallets.deposit(token, Money::zero()), ValidationError);
    CHECK_THROWS_AS(f.wallets.deposit(token, Money::fromPaise(-10000)), ValidationError);
    CHECK_THROWS_AS(f.wallets.deposit(token, Money::fromPaise(limits.minDeposit.paise() - 1)), ValidationError);
    CHECK_THROWS_AS(f.wallets.deposit(token, Money::fromPaise(limits.maxDeposit.paise() + 1)), ValidationError);
    CHECK_THROWS_AS(f.wallets.deposit(token, Money::fromPaise(std::numeric_limits<std::int64_t>::max())),
                    ValidationError);
    CHECK_EQ(f.count("SELECT count(*) FROM transactions"), 0);
    CHECK(f.wallets.balance(token).isZero());

    // The boundaries themselves are allowed.
    CHECK(f.wallets.deposit(token, limits.minDeposit).succeeded());
    CHECK(f.wallets.deposit(token, limits.maxDeposit).succeeded());
}

TEST(deposit_over_the_balance_cap_is_recorded_as_failed) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    const WalletLimits& limits = f.wallets.limits();

    CHECK(f.wallets.deposit(token, limits.maxDeposit).succeeded());
    CHECK(f.wallets.deposit(token, limits.maxDeposit).succeeded()); // exactly at the cap
    CHECK_EQ(f.wallets.balance(token), limits.maxBalance);

    const auto rejected = f.wallets.deposit(token, rupees("1"));
    CHECK(!rejected.succeeded());
    CHECK(rejected.isPersisted());
    CHECK(rejected.failureReason().find("limit") != std::string::npos);
    CHECK_EQ(f.wallets.balance(token), limits.maxBalance); // unchanged

    CHECK_EQ(f.count("SELECT count(*) FROM transactions WHERE status = 'FAILED'"), 1);
    TransactionRepository transactions(f.db);
    CHECK_EQ(transactions.ledgerBalance(f.wallets.myWallet(token).id()), limits.maxBalance);
}

TEST(deposit_is_atomic_when_the_audit_record_cannot_be_written) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    f.db.connection().execute("CREATE TRIGGER no_audit BEFORE INSERT ON transactions "
                              "BEGIN SELECT RAISE(ABORT, 'audit log unavailable'); END");

    CHECK_THROWS_AS(f.wallets.deposit(token, rupees("100")), PaySphereError);
    CHECK(f.wallets.balance(token).isZero()); // the balance update was rolled back

    f.db.connection().execute("DROP TRIGGER no_audit");
    CHECK(f.wallets.deposit(token, rupees("100")).succeeded());
    CHECK_EQ(f.wallets.balance(token), rupees("100"));
}

TEST(deposit_is_atomic_when_the_balance_cannot_be_written) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    f.db.connection().execute("CREATE TRIGGER frozen BEFORE UPDATE ON wallets "
                              "BEGIN SELECT RAISE(ABORT, 'wallet frozen'); END");

    CHECK_THROWS_AS(f.wallets.deposit(token, rupees("100")), PaySphereError);
    CHECK_EQ(f.count("SELECT count(*) FROM transactions"), 0); // no orphan audit entry
    CHECK(f.wallets.balance(token).isZero());
}

TEST(deposit_retries_when_a_reference_collides) {
    ScriptedReferenceGenerator references({"DEP-COLLIDE", "DEP-COLLIDE", "DEP-FRESH"});
    WalletFixture f(":memory:", &references);
    const std::string token = f.signUp("asha", "9876543210");

    CHECK_EQ(f.wallets.deposit(token, rupees("10")).reference(), "DEP-COLLIDE");
    CHECK_EQ(f.wallets.deposit(token, rupees("20")).reference(), "DEP-FRESH");
    CHECK_EQ(f.wallets.balance(token), rupees("30")); // the failed attempt left no trace
    CHECK_EQ(f.count("SELECT count(*) FROM transactions"), 2);
}

TEST(deposit_gives_up_after_repeated_collisions) {
    ScriptedReferenceGenerator references({"DEP-SAME", "DEP-SAME", "DEP-SAME", "DEP-SAME"});
    WalletFixture f(":memory:", &references);
    const std::string token = f.signUp("asha", "9876543210");

    f.wallets.deposit(token, rupees("10"));
    CHECK_THROWS_AS(f.wallets.deposit(token, rupees("20")), ConstraintViolation);
    CHECK_EQ(f.wallets.balance(token), rupees("10"));
    CHECK_EQ(f.count("SELECT count(*) FROM transactions"), 1);
}

TEST(transaction_references_are_unique) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    std::set<std::string> references;
    for (int i = 0; i < 100; ++i) {
        references.insert(f.wallets.deposit(token, rupees("1")).reference());
    }
    CHECK_EQ(references.size(), 100u);
    CHECK_EQ(f.wallets.balance(token), rupees("100"));
}

// ------------------------------------------ protection against bad writes

TEST(balance_rejects_stale_negative_and_unrecorded_writes) {
    WalletFixture f;
    const std::string token = f.signUp("asha", "9876543210");
    f.wallets.deposit(token, rupees("100"));
    const Wallet wallet = f.wallets.myWallet(token);
    WalletRepository repository(f.db);

    // A write based on a stale balance is refused (compare-and-set).
    CHECK_THROWS_AS(repository.updateBalance(wallet.id(), rupees("50"), rupees("999")), DatabaseError);
    // A negative balance is refused by the schema, even through raw SQL.
    CHECK_THROWS_AS(repository.updateBalance(wallet.id(), wallet.balance(), Money::fromPaise(-1)),
                    ConstraintViolation);
    CHECK_THROWS_AS(f.db.execute("UPDATE wallets SET balance_paise = -1"), ConstraintViolation);
    CHECK_EQ(f.wallets.balance(token), rupees("100"));

    // A change made behind the service's back (no audit entry) is caught by
    // reconciling the stored balance against the transaction ledger.
    TransactionRepository transactions(f.db);
    CHECK_EQ(transactions.ledgerBalance(wallet.id()), f.wallets.balance(token));
    f.db.execute("UPDATE wallets SET balance_paise = balance_paise + 100000 WHERE id = ?", {wallet.id()});
    CHECK(transactions.ledgerBalance(wallet.id()) != f.wallets.balance(token));
}

TEST(deposits_persist_across_restarts) {
    TempDirectory directory;
    const std::string path = directory.file("paysphere.db");
    std::string reference;
    {
        WalletFixture first(path);
        const std::string token = first.signUp("asha", "9876543210");
        first.wallets.deposit(token, rupees("750.25"));
        reference = first.wallets.deposit(token, rupees("249.75")).reference();
    } // the "application" shuts down; sessions are gone

    WalletFixture second(path);
    CHECK_THROWS_AS(second.wallets.balance("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
                    SessionError);
    const std::string token = second.login("asha");
    CHECK_EQ(second.wallets.balance(token), rupees("1000"));
    TransactionRepository transactions(second.db);
    const auto stored = transactions.findByReference(reference);
    CHECK(stored.has_value());
    CHECK_EQ(stored->amount(), rupees("249.75"));
}
