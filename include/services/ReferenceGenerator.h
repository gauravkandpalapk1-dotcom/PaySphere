#pragma once

#include "models/Transaction.h"
#include "services/Clock.h"

#include <string>

namespace paysphere::services {

// Creates the public transaction IDs shown on receipts. Abstract, so tests
// can force a collision and check that it is handled.
class ReferenceGenerator {
public:
    virtual ~ReferenceGenerator();
    virtual std::string next(models::TransactionType type, TimePoint now) = 0;
};

// "DEP20261014" + 12 uppercase hex digits, e.g. DEP20261014A3F09C1B77D2.
//   - The prefix gives the type (DEP for deposits, TRF for transfers).
//   - The UTC date makes support queries easy.
//   - 48 bits from libsodium's CSPRNG make IDs unguessable, so one receipt
//     does not reveal the IDs of other people's transactions.
// The database's UNIQUE constraint remains the guarantee of uniqueness.
class RandomReferenceGenerator final : public ReferenceGenerator {
public:
    RandomReferenceGenerator();
    std::string next(models::TransactionType type, TimePoint now) override;
};

} // namespace paysphere::services
