#pragma once

#include "database/TransactionRepository.h"
#include "database/WalletRepository.h"
#include "models/Transaction.h"
#include "models/User.h"
#include "models/Wallet.h"
#include "payments/Money.h"
#include "services/IdempotencyLedger.h"
#include "services/WalletLimits.h"

#include <cstdint>
#include <string>

namespace paysphere::db {
class DatabaseManager;
}

namespace paysphere::services {

class AuthService;
class Clock;
class ReferenceGenerator;

// Wallet access and simulated deposits for the logged-in user.
//
// Authorization model: every method takes a session token and works only on
// the wallet of the user that token belongs to. There is no way to name
// another user's wallet except walletById(), which refuses it.
class WalletService {
public:
    WalletService(db::DatabaseManager& database, AuthService& auth, ReferenceGenerator& references,
                  const Clock& clock, WalletLimits limits = {});

    // The caller's wallet. Throws SessionError for a bad or expired token.
    models::Wallet myWallet(const std::string& sessionToken);
    payments::Money balance(const std::string& sessionToken);
    // A wallet by id, only if it belongs to the caller; otherwise (including
    // when no such wallet exists) throws AuthorizationError.
    models::Wallet walletById(const std::string& sessionToken, std::int64_t walletId);

    // Adds simulated money (no real bank is involved) to the caller's wallet.
    //
    // Invalid amounts (not positive, below minDeposit, above maxDeposit)
    // throw ValidationError, and nothing is recorded.
    //
    // A valid deposit always leaves an audit record, returned to the caller:
    //   - SUCCESS: the balance was increased in the same transaction;
    //   - FAILED:  the balance would have exceeded maxBalance, so it was left
    //              unchanged and the reason is in failureReason().
    // If anything fails part-way, everything is rolled back.
    //
    // idempotencyKey (see IdempotencyLedger): repeating a deposit with the
    // same key and amount returns the original transaction instead of
    // crediting again, so a client can safely retry after a timeout. The
    // overload without a key treats every call as a new request.
    models::Transaction deposit(const std::string& sessionToken, payments::Money amount,
                                const std::string& idempotencyKey);
    models::Transaction deposit(const std::string& sessionToken, payments::Money amount);

    const WalletLimits& limits() const noexcept { return limits_; }

private:
    void validateDepositAmount(payments::Money amount) const;

    db::DatabaseManager& database_;
    AuthService& auth_;
    ReferenceGenerator& references_;
    const Clock& clock_;
    WalletLimits limits_;
    db::WalletRepository wallets_;
    db::TransactionRepository transactions_;
    IdempotencyLedger idempotency_;
};

} // namespace paysphere::services
