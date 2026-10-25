#include "services/WalletService.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "services/AuthService.h"
#include "services/Clock.h"
#include "services/ReferenceGenerator.h"

#include <optional>

namespace paysphere::services {

namespace {

// A clash between two random 48-bit IDs on the same day is extremely
// unlikely; retrying a few times turns "unlikely" into "never seen".
constexpr int kMaxReferenceAttempts = 3;

} // namespace

WalletService::WalletService(db::DatabaseManager& database, AuthService& auth,
                             ReferenceGenerator& references, const Clock& clock, WalletLimits limits)
    : database_(database),
      auth_(auth),
      references_(references),
      clock_(clock),
      limits_(limits),
      wallets_(database),
      transactions_(database),
      idempotency_(database) {
    if (!limits_.minDeposit.isPositive() || limits_.maxDeposit < limits_.minDeposit ||
        limits_.maxBalance < limits_.maxDeposit) {
        throw ValidationError("Wallet limits must satisfy 0 < minDeposit <= maxDeposit <= maxBalance");
    }
}

models::Wallet WalletService::myWallet(const std::string& sessionToken) {
    return wallets_.requireByUserId(auth_.authenticate(sessionToken).id());
}

payments::Money WalletService::balance(const std::string& sessionToken) {
    return myWallet(sessionToken).balance();
}

models::Wallet WalletService::walletById(const std::string& sessionToken, std::int64_t walletId) {
    const models::User user = auth_.authenticate(sessionToken);
    std::optional<models::Wallet> wallet = wallets_.findById(walletId);
    if (!wallet || !wallet->belongsTo(user.id())) {
        throw AuthorizationError("You do not have access to this wallet");
    }
    return *wallet;
}

void WalletService::validateDepositAmount(payments::Money amount) const {
    if (!amount.isPositive()) {
        throw ValidationError("Deposit amount must be greater than zero");
    }
    if (amount < limits_.minDeposit) {
        throw ValidationError("The minimum deposit is " + limits_.minDeposit.toString());
    }
    if (amount > limits_.maxDeposit) {
        throw ValidationError("A single deposit cannot exceed " + limits_.maxDeposit.toString());
    }
}

models::Transaction WalletService::deposit(const std::string& sessionToken, payments::Money amount) {
    return deposit(sessionToken, amount, IdempotencyLedger::newKey());
}

models::Transaction WalletService::deposit(const std::string& sessionToken, payments::Money amount,
                                           const std::string& idempotencyKey) {
    // Authenticate first: an anonymous caller learns nothing, not even
    // whether the amount was valid.
    const models::User user = auth_.authenticate(sessionToken);
    validateDepositAmount(amount);
    IdempotencyLedger::validateKey(idempotencyKey);
    const std::string fingerprint = IdempotencyLedger::fingerprint("DEPOSIT\n" + std::to_string(amount.paise()));

    for (int attempt = 1;; ++attempt) {
        const TimePoint now = clock_.now();
        const std::string reference = references_.next(models::TransactionType::Deposit, now);
        try {
            return database_.inTransaction([&] {
                // A retry of an earlier identical deposit gets the original back.
                if (std::optional<models::Transaction> original =
                        idempotency_.replay(user.id(), idempotencyKey, fingerprint)) {
                    return *original;
                }

                // Read inside the transaction: BEGIN IMMEDIATE holds the
                // write lock, so the balance cannot change before the update.
                const models::Wallet wallet = wallets_.requireByUserId(user.id());
                const bool fits = wallet.canReceive(amount, limits_.maxBalance);
                if (fits) {
                    wallets_.updateBalance(wallet.id(), wallet.balance(), wallet.balance() + amount);
                }
                const std::string createdAt = formatTimestamp(now);
                const models::Transaction recorded = transactions_.insert(models::Transaction(
                    0, reference, models::TransactionType::Deposit,
                    fits ? models::TransactionStatus::Success : models::TransactionStatus::Failed, std::nullopt,
                    wallet.id(), amount,
                    fits ? std::string()
                         : "Deposit would take the balance above the wallet limit of " +
                               limits_.maxBalance.toString(),
                    createdAt));
                idempotency_.remember(user.id(), idempotencyKey, fingerprint, recorded.id(), createdAt);
                return recorded;
            });
        } catch (const db::ConstraintViolation& error) {
            // The whole transaction, balance update included, has been rolled
            // back, so retrying is safe: the next attempt gets a fresh
            // reference, or finds the replay if the same key was committed
            // concurrently.
            if (!error.isUniqueViolation() || attempt == kMaxReferenceAttempts) {
                throw;
            }
        }
    }
}

} // namespace paysphere::services
