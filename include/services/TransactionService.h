#pragma once

#include "database/TransactionRepository.h"
#include "database/WalletRepository.h"
#include "models/Transaction.h"
#include "models/TransactionDetails.h"
#include "models/Wallet.h"
#include "payments/Money.h"
#include "services/Receipt.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace paysphere::db {
class DatabaseManager;
}

namespace paysphere::services {

class AuthService;

struct HistoryQuery {
    std::optional<models::TransactionDirection> direction; // nullopt = sent and received
    std::optional<models::TransactionStatus> status;       // nullopt = any status
    std::int64_t limit = 20;                               // 1 to 100
    std::int64_t offset = 0;
};

// One line of a user's history.
struct HistoryEntry {
    models::Transaction transaction;
    models::TransactionDirection direction;
    // The other user: the payee for a sent transfer, the payer for a received
    // one. Empty for deposits and for transfers to an unknown UPI ID.
    std::optional<models::Party> counterparty;
};

struct HistoryPage {
    std::vector<HistoryEntry> entries; // newest first
    std::int64_t totalMatching = 0;    // across all pages
};

// Recomputes the wallet balance from the audit trail.
struct LedgerAudit {
    payments::Money storedBalance;
    payments::Money ledgerBalance;
    bool consistent() const noexcept { return storedBalance == ledgerBalance; }
};

// Read access to the transaction audit trail. Deposits (WalletService) and
// UPI transfers (PaymentService) write the records; this service only reads
// them, and there is deliberately no way to change one (the database also
// refuses UPDATE and DELETE on transactions).
//
// Access control: every call is tied to a session and sees only
// transactions visible to the caller's own wallet (Transaction::isVisibleTo).
class TransactionService {
public:
    static constexpr std::int64_t kMaxPageSize = 100;

    TransactionService(db::DatabaseManager& database, AuthService& auth);

    // Throws SessionError or ValidationError (bad limit/offset).
    HistoryPage history(const std::string& sessionToken, const HistoryQuery& query = {});

    // Search by transaction ID (case-insensitive, surrounding spaces ignored).
    // nullopt if no such transaction exists *or* the caller may not see it;
    // the two cases are indistinguishable on purpose.
    std::optional<HistoryEntry> findByReference(const std::string& sessionToken, const std::string& reference);

    // Throws AuthorizationError if the transaction is unknown or not visible
    // to the caller (same error for both).
    Receipt receipt(const std::string& sessionToken, const std::string& reference);

    // Compares the caller's stored balance with the one implied by the audit trail.
    LedgerAudit audit(const std::string& sessionToken);

private:
    models::Wallet callerWallet(const std::string& sessionToken);
    // nullopt unless the transaction is visible to the wallet.
    std::optional<models::TransactionDetails> visibleDetails(std::int64_t walletId, const std::string& reference);
    static HistoryEntry toEntry(models::TransactionDetails details, std::int64_t walletId);

    db::DatabaseManager& database_;
    AuthService& auth_;
    db::WalletRepository wallets_;
    db::TransactionRepository transactions_;
};

} // namespace paysphere::services
