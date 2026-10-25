#pragma once

#include "database/DatabaseManager.h"
#include "services/AuthService.h"
#include "services/Clock.h"
#include "services/PasswordHasher.h"
#include "services/PaymentService.h"
#include "services/ReferenceGenerator.h"
#include "services/RiskEngine.h"
#include "services/SessionManager.h"
#include "services/TransactionService.h"
#include "services/UpiPinService.h"
#include "services/WalletService.h"

#include <chrono>
#include <string>

namespace paysphere::services {

struct ApplicationOptions {
    // Argon2id cost for passwords and UPI PINs. Tests lower it for speed.
    SodiumPasswordHasher::Cost passwordCost = SodiumPasswordHasher::Cost::interactive();
    // Time source; nullptr means the system clock. Tests pass a manual clock.
    const Clock* clock = nullptr;
    // How long to wait for another process's database lock before failing.
    std::chrono::milliseconds busyTimeout = db::Database::kDefaultBusyTimeout;
};

// Composition root: builds the whole application once and wires every
// service to its collaborators through constructor injection. Front ends
// (the CLI in main.cpp, the end-to-end tests) talk only to the services
// exposed here and contain no business rules of their own.
//
// Members are declared in dependency order, which is also their
// construction order.
class ApplicationContext {
public:
    explicit ApplicationContext(const std::string& databasePath, ApplicationOptions options = {});

    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;

    db::DatabaseManager& database() noexcept { return database_; }
    const db::DatabaseManager& database() const noexcept { return database_; }
    const Clock& clock() const noexcept { return clock_; }
    AuthService& auth() noexcept { return auth_; }
    SessionManager& sessions() noexcept { return sessions_; }
    WalletService& wallets() noexcept { return wallets_; }
    UpiPinService& pins() noexcept { return pins_; }
    RiskEngine& risk() noexcept { return risk_; }
    PaymentService& payments() noexcept { return payments_; }
    TransactionService& transactions() noexcept { return transactions_; }

private:
    db::DatabaseManager database_;
    SystemClock systemClock_;
    const Clock& clock_;
    SodiumPasswordHasher passwordHasher_;
    SessionManager sessions_;
    AuthService auth_;
    RandomReferenceGenerator references_;
    WalletService wallets_;
    UpiPinService pins_;
    RiskEngine risk_;
    PaymentService payments_;
    TransactionService transactions_;
};

} // namespace paysphere::services
