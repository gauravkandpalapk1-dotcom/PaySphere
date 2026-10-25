#pragma once

#include "models/Wallet.h"
#include "payments/Money.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

class DatabaseManager;
class Statement;

// Persistence for the wallets table.
class WalletRepository {
public:
    explicit WalletRepository(DatabaseManager& database);

    // Creates the user's wallet with a zero balance and returns it. An empty
    // createdAt means "now" according to the database.
    // Throws ConstraintViolation if the user already has one.
    models::Wallet createForUser(std::int64_t userId, const std::string& createdAt = {});

    std::optional<models::Wallet> findById(std::int64_t walletId);
    std::optional<models::Wallet> findByUserId(std::int64_t userId);
    // Like findByUserId, but every account is expected to have a wallet
    // (registration creates both atomically): throws CorruptDataError if not.
    models::Wallet requireByUserId(std::int64_t userId);

    // The only way a balance is written. It is a compare-and-set: the update
    // happens only if the stored balance still equals expectedBalance, so a
    // decision made on a stale read can never be applied. Throws
    // ConcurrentModificationError if the balance had changed (nothing is
    // written), and ConstraintViolation if newBalance is negative.
    //
    // Call it only inside a transaction that also records the matching
    // audit entry in the transactions table (see WalletService).
    void updateBalance(std::int64_t walletId, payments::Money expectedBalance,
                       payments::Money newBalance);

private:
    static models::Wallet toWallet(const Statement& row);

    DatabaseManager& database_;
};

} // namespace paysphere::db
