#include "models/Wallet.h"

#include "models/Errors.h"

#include <utility>

namespace paysphere::models {

Wallet::Wallet(std::int64_t id,
               std::int64_t userId,
               payments::Money balance,
               std::string createdAt)
    : id_(id),
      userId_(userId),
      balance_(balance),
      createdAt_(std::move(createdAt)) {
    if (id_ < 0) {
        throw ValidationError("Wallet id must not be negative");
    }
    if (userId_ <= 0) {
        throw ValidationError("Wallet must belong to a saved user");
    }
    if (balance_.isNegative()) {
        throw ValidationError("Wallet balance must not be negative");
    }
}

bool Wallet::canAfford(payments::Money amount) const noexcept {
    return amount.isPositive() && amount <= balance_;
}

payments::Money Wallet::headroom(payments::Money maxBalance) const noexcept {
    // Both values are non-negative here, so the subtraction cannot overflow.
    if (maxBalance <= balance_) {
        return payments::Money::zero();
    }
    return payments::Money::fromPaise(maxBalance.paise() - balance_.paise());
}

bool Wallet::canReceive(payments::Money amount, payments::Money maxBalance) const noexcept {
    // Compared against the headroom rather than computing balance + amount,
    // which could overflow for absurd amounts.
    return amount.isPositive() && amount <= headroom(maxBalance);
}

} // namespace paysphere::models
