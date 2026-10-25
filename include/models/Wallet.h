#pragma once

#include "payments/Money.h"

#include <cstdint>
#include <string>

namespace paysphere::models {

// A user's wallet: a read-only snapshot of one row of the wallets table.
//
// The balance is encapsulated: there are no setters, and nothing in the
// program can change a Wallet object's balance. A balance changes only when
// services::WalletService updates the database inside an atomic transaction
// that also records the matching audit entry; callers then read a fresh
// snapshot. The balance rules themselves live here, next to the data.
class Wallet {
public:
    Wallet(std::int64_t id,
           std::int64_t userId,
           payments::Money balance,
           std::string createdAt = {});

    std::int64_t id() const noexcept { return id_; }
    bool isPersisted() const noexcept { return id_ > 0; }
    std::int64_t userId() const noexcept { return userId_; }
    payments::Money balance() const noexcept { return balance_; }
    const std::string& createdAt() const noexcept { return createdAt_; }

    bool belongsTo(std::int64_t userId) const noexcept { return userId_ == userId; }

    // True if amount is positive and does not exceed the balance (for spending).
    bool canAfford(payments::Money amount) const noexcept;
    // How much more the wallet may hold before reaching maxBalance (never negative).
    payments::Money headroom(payments::Money maxBalance) const noexcept;
    // True if amount is positive and fits under maxBalance (for receiving money).
    bool canReceive(payments::Money amount, payments::Money maxBalance) const noexcept;

private:
    std::int64_t id_;
    std::int64_t userId_;
    payments::Money balance_;
    std::string createdAt_;
};

} // namespace paysphere::models
