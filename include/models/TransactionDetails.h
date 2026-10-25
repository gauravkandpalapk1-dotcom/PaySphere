#pragma once

#include "models/Transaction.h"

#include <optional>
#include <string>

namespace paysphere::models {

// A user on one side of a transaction, as shown in history and receipts.
struct Party {
    std::string fullName;
    std::string upiId;
};

// A transaction with both parties resolved to people. `from` is empty for
// deposits; `to` is empty for a transfer whose recipient was never found
// (Transaction::payeeUpiId() still says who was meant to be paid).
struct TransactionDetails {
    Transaction transaction;
    std::optional<Party> from;
    std::optional<Party> to;
};

} // namespace paysphere::models
