// End-to-end workflows through the real composition root (ApplicationContext)
// on a database file, including full application restarts.

#include "TestFramework.h"
#include "TestSupport.h"

#include "models/Errors.h"
#include "payments/Money.h"
#include "payments/UPIPayment.h"
#include "services/ApplicationContext.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using paysphere::AccountLockedError;
using paysphere::AuthorizationError;
using paysphere::PaymentBlockedError;
using paysphere::SessionError;
using paysphere::ValidationError;
using paysphere::models::TransactionStatus;
using paysphere::payments::Money;
using paysphere::payments::UPIPayment;
using paysphere::services::ApplicationContext;
using paysphere::services::ApplicationOptions;
using paysphere::services::PaymentResult;
using paysphere::services::PaymentService;
using paysphere::services::RiskContext;
using paysphere::services::RiskRule;
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

// One installation of the application: a database file that outlives any
// number of application runs.
struct Deployment {
    TempDirectory directory;
    ManualClock clock;
    std::string path = directory.file("paysphere.db");
    std::unique_ptr<ApplicationContext> app;

    Deployment() { start(); }

    void start() {
        app = std::make_unique<ApplicationContext>(
            path, ApplicationOptions{SodiumPasswordHasher::Cost::minimumForTests(), &clock});
    }
    void restart() {
        app.reset(); // closes the database; in-memory sessions are lost
        start();
    }

    void registerUser(const std::string& name, const std::string& fullName, const std::string& phone) {
        app->auth().registerUser({fullName, name + "@example.com", phone, name + "@paysphere", kPassword});
    }
    std::string login(const std::string& name) {
        return app->auth().login(name + "@example.com", kPassword).sessionToken;
    }
    std::string onboard(const std::string& name, const std::string& fullName, const std::string& phone) {
        registerUser(name, fullName, phone);
        const std::string token = login(name);
        app->pins().setPin(token, kPin);
        return token;
    }
    PaymentResult pay(const std::string& token, const std::string& payee, const char* amount,
                      const std::string& key = PaymentService::newIdempotencyKey(),
                      const std::string& pin = kPin) {
        const UPIPayment payment(payee, rupees(amount), pin);
        return app->payments().pay(token, payment, key);
    }
    Money totalMoney() {
        return Money::fromPaise(app->database().queryInt64("SELECT sum(balance_paise) FROM wallets").value());
    }
};

} // namespace

TEST(e2e_complete_user_journey) {
    Deployment d;
    // Register and log in.
    const std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    const std::string ravi = d.onboard("ravi", "Ravi Kumar", "9123456789");

    // Deposit and check the balance.
    CHECK(d.app->wallets().deposit(asha, rupees("1000")).succeeded());
    CHECK_EQ(d.app->wallets().balance(asha), rupees("1000"));

    // Pay by UPI ID.
    const PaymentResult paid = d.pay(asha, "ravi@paysphere", "250.50");
    CHECK(paid.transaction.succeeded());
    CHECK_EQ(d.app->wallets().balance(asha), rupees("749.50"));
    CHECK_EQ(d.app->wallets().balance(ravi), rupees("250.50"));

    // Both sides see it in their history and can print a receipt.
    CHECK_EQ(d.app->transactions().history(asha).totalMatching, 2);
    CHECK_EQ(d.app->transactions().history(ravi).entries.front().transaction.reference(),
             paid.transaction.reference());
    const std::string receipt = d.app->transactions().receipt(ravi, paid.transaction.reference()).toText();
    CHECK(receipt.find("From            Asha Verma <asha@paysphere>") != std::string::npos);

    // Log out: the token stops working in every service.
    d.app->auth().logout(asha);
    CHECK_THROWS_AS(d.app->wallets().balance(asha), SessionError);
    CHECK_THROWS_AS(d.pay(asha, "ravi@paysphere", "1"), SessionError);
    CHECK_THROWS_AS(d.app->transactions().history(asha), SessionError);
    CHECK_THROWS_AS(d.app->pins().setPin(asha, "2580"), SessionError);
    CHECK(d.app->transactions().audit(ravi).consistent());
}

TEST(e2e_everything_but_sessions_survives_a_restart) {
    Deployment d;
    std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    d.onboard("ravi", "Ravi Kumar", "9123456789");
    d.app->wallets().deposit(asha, rupees("1000"));
    const PaymentResult paid = d.pay(asha, "ravi@paysphere", "300", "restart-key-0001");
    const std::string receiptBefore = d.app->transactions().receipt(asha, paid.transaction.reference()).toText();

    d.restart();

    // Sessions are in memory, so the old token is gone...
    CHECK_THROWS_AS(d.app->wallets().balance(asha), SessionError);
    asha = d.login("asha");
    // ...but accounts, balances, history and receipts are persistent.
    CHECK_EQ(d.app->wallets().balance(asha), rupees("700"));
    CHECK_EQ(d.app->transactions().history(asha).totalMatching, 2);
    CHECK_EQ(d.app->transactions().receipt(asha, paid.transaction.reference()).toText(), receiptBefore);
    CHECK(d.app->transactions().audit(asha).consistent());

    // The PIN is persistent: no need to set it again.
    CHECK(d.pay(asha, "ravi@paysphere", "1").transaction.succeeded());
    // Idempotency is persistent: a retry after the restart is not paid twice.
    const PaymentResult retry = d.pay(asha, "ravi@paysphere", "300", "restart-key-0001");
    CHECK(retry.replayed);
    CHECK_EQ(retry.transaction.id(), paid.transaction.id());
    CHECK_EQ(d.app->wallets().balance(asha), rupees("699"));
}

TEST(e2e_security_state_survives_a_restart) {
    Deployment d;
    std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    d.onboard("ravi", "Ravi Kumar", "9123456789");
    d.app->wallets().deposit(asha, rupees("100"));
    for (int i = 0; i < 3; ++i) {
        d.pay(asha, "ravi@paysphere", "1", PaymentService::newIdempotencyKey(), "2468"); // wrong PIN
    }
    for (int i = 0; i < 5; ++i) {
        try {
            d.app->auth().login("ravi@example.com", "wrong password 1");
        } catch (const paysphere::AuthenticationError&) {
        }
    }

    d.restart();

    // Restarting does not reset the PIN block or the login lockout.
    asha = d.login("asha");
    CHECK_THROWS_AS(d.pay(asha, "ravi@paysphere", "1"), PaymentBlockedError);
    CHECK_THROWS_AS(d.login("ravi"), AccountLockedError);
    CHECK_EQ(d.app->wallets().balance(asha), rupees("100"));
}

TEST(e2e_authorization_holds_across_services) {
    Deployment d;
    const std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    const std::string ravi = d.onboard("ravi", "Ravi Kumar", "9123456789");
    d.app->wallets().deposit(asha, rupees("100"));
    const auto failed = d.pay(asha, "ravi@paysphere", "500").transaction; // insufficient funds

    // Ravi cannot see asha's wallet or her failed attempt to pay him.
    CHECK_THROWS_AS(d.app->wallets().walletById(ravi, d.app->wallets().myWallet(asha).id()), AuthorizationError);
    CHECK(!d.app->transactions().findByReference(ravi, failed.reference()).has_value());
    CHECK_THROWS_AS(d.app->transactions().receipt(ravi, failed.reference()), AuthorizationError);

    // An idle session expires everywhere at once.
    d.clock.advance(31min);
    CHECK_THROWS_AS(d.app->wallets().balance(asha), SessionError);
    CHECK_THROWS_AS(d.app->transactions().history(ravi), SessionError);
    CHECK_EQ(d.app->sessions().activeCount(), 0u);
}

TEST(e2e_failures_leave_the_system_consistent) {
    Deployment d;
    const std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    const std::string ravi = d.onboard("ravi", "Ravi Kumar", "9123456789");
    d.app->wallets().deposit(asha, rupees("500"));
    d.app->wallets().deposit(ravi, rupees("500"));

    // Errors that are rejected outright...
    CHECK_THROWS_AS(d.app->auth().registerUser({"Asha Two", "ASHA@example.com", "9000000001", "asha2@paysphere",
                                                kPassword}),
                    paysphere::DuplicateAccountError);
    CHECK_THROWS_AS(d.app->wallets().deposit(asha, Money::zero()), ValidationError);
    CHECK_THROWS_AS(d.pay(asha, "asha@paysphere", "10"), ValidationError);
    CHECK_THROWS_AS(Money::parse("12.345"), ValidationError);
    d.pay(asha, "ravi@paysphere", "10", "conflict-key-01");
    CHECK_THROWS_AS(d.pay(asha, "ravi@paysphere", "20", "conflict-key-01"), paysphere::IdempotencyConflictError);
    // ...and failures that are recorded.
    CHECK(!d.pay(asha, "ravi@paysphere", "10000").transaction.succeeded());
    CHECK(!d.pay(asha, "nobody@paysphere", "10").transaction.succeeded());
    CHECK(!d.pay(asha, "ravi@paysphere", "10", PaymentService::newIdempotencyKey(), "2468").transaction.succeeded());

    paysphere::services::HistoryQuery failedOnly;
    failedOnly.status = TransactionStatus::Failed;
    CHECK_EQ(d.app->transactions().history(asha, failedOnly).totalMatching, 3);
    CHECK_EQ(d.totalMoney(), rupees("1000")); // nothing created or destroyed
    CHECK(d.app->transactions().audit(asha).consistent());
    CHECK(d.app->transactions().audit(ravi).consistent());
}

TEST(e2e_services_share_one_composition_root) {
    Deployment d;
    const std::string asha = d.onboard("asha", "Asha Verma", "9876543210");
    d.onboard("ravi", "Ravi Kumar", "9123456789");
    d.app->wallets().deposit(asha, rupees("100"));

    // A rule added to the application's RiskEngine is used by its PaymentService.
    class NoPaymentsOverTen final : public RiskRule {
    public:
        const char* name() const noexcept override { return "demo-cap"; }
        std::optional<std::string> check(const RiskContext& context) const override {
            if (context.amount > Money::parse("10")) {
                return std::string("Demo cap of Rs. 10 exceeded");
            }
            return std::nullopt;
        }
    };
    d.app->risk().addRule(std::make_unique<NoPaymentsOverTen>());
    CHECK(d.pay(asha, "ravi@paysphere", "10").transaction.succeeded());
    CHECK(d.pay(asha, "ravi@paysphere", "11").transaction.failureReason().find("Demo cap") != std::string::npos);

    // AuthService and WalletService share one SessionManager.
    CHECK_EQ(d.app->sessions().activeCount(), 2u);
    d.app->auth().logout(asha);
    CHECK_EQ(d.app->sessions().activeCount(), 1u);
    CHECK_THROWS_AS(d.app->wallets().balance(asha), SessionError);
}
