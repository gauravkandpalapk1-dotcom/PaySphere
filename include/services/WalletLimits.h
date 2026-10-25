#pragma once

#include "payments/Money.h"

namespace paysphere::services {

// Limits on simulated deposits and on how much a wallet may hold.
struct WalletLimits {
    payments::Money minDeposit = payments::Money::fromPaise(1'00);         // Rs. 1
    payments::Money maxDeposit = payments::Money::fromPaise(1'00'000'00);  // Rs. 1,00,000 per deposit
    // Rs. 2,00,000: modelled on the RBI's outstanding-balance cap for
    // full-KYC prepaid wallets (check the current RBI Master Direction on
    // PPIs before citing it). Applies to deposits and incoming transfers.
    payments::Money maxBalance = payments::Money::fromPaise(2'00'000'00);
};

} // namespace paysphere::services
