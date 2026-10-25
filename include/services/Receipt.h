#pragma once

#include "models/Transaction.h"
#include "models/TransactionDetails.h"

#include <string>

namespace paysphere::services {

// A printable record of one transaction, as seen by one of its parties.
// Built only by TransactionService, after checking that the viewer may see
// the transaction.
class Receipt {
public:
    Receipt(models::TransactionDetails details, models::TransactionDirection viewerDirection);

    const models::Transaction& transaction() const noexcept { return details_.transaction; }
    models::TransactionDirection viewerDirection() const noexcept { return viewerDirection_; }

    // Plain text, one "label  value" line per field, e.g.
    //   Transaction ID  TRF20261014A3F09C1B77D2
    //   Status          SUCCESS
    //   Amount          Rs. 250.50
    std::string toText() const;

private:
    models::TransactionDetails details_;
    models::TransactionDirection viewerDirection_;
};

} // namespace paysphere::services
