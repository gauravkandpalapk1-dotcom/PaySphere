#pragma once

#include "database/SqlValue.h"
#include "models/Transaction.h"
#include "models/TransactionDetails.h"
#include "payments/Money.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace paysphere::db {

class DatabaseManager;
class Statement;

// Which of a wallet's visible transactions to return, newest first.
struct HistoryFilter {
    std::optional<models::TransactionDirection> direction; // nullopt = both
    std::optional<models::TransactionStatus> status;       // nullopt = both
    std::int64_t limit = 20;
    std::int64_t offset = 0;
};

// Persistence for the transactions table: the audit trail of every money
// movement, successful or failed. Rows are only ever inserted; nothing here
// updates or deletes them (and the schema forbids it).
class TransactionRepository {
public:
    explicit TransactionRepository(DatabaseManager& database);

    // Inserts the transaction (its id must be 0) and returns the stored row.
    // An empty createdAt means "now" according to the database.
    // Throws ConstraintViolation for a duplicate reference.
    models::Transaction insert(const models::Transaction& transaction);

    std::optional<models::Transaction> findById(std::int64_t id);
    std::optional<models::Transaction> findByReference(const std::string& reference);

    // Recomputes a wallet's balance from its successful transactions
    // (money in minus money out). An audit can compare this with the stored
    // balance: any difference means the balance changed without a record.
    payments::Money ledgerBalance(std::int64_t walletId);

    // Transfers sent from a wallet with created_at after `since` (used by
    // risk checks). `oldestAt` is the earliest counted one, if any.
    struct TransferSummary {
        std::int64_t count = 0;
        payments::Money total;
        std::optional<std::string> oldestAt;
    };
    TransferSummary successfulTransfersSince(std::int64_t walletId, const std::string& since);
    TransferSummary failedTransfersSince(std::int64_t walletId, const std::string& since);
    // When the first successful transfer from one wallet to another happened.
    std::optional<std::string> firstSuccessfulTransferBetween(std::int64_t fromWalletId,
                                                              std::int64_t toWalletId);

    // The page of transactions visible to a wallet (see
    // Transaction::isVisibleTo) that match the filter, newest first.
    std::vector<models::TransactionDetails> history(std::int64_t walletId, const HistoryFilter& filter);
    // How many visible transactions match the filter (ignores limit/offset).
    std::int64_t countHistory(std::int64_t walletId, const HistoryFilter& filter);
    // Any transaction by reference. The caller must check visibility.
    std::optional<models::TransactionDetails> findDetailsByReference(const std::string& reference);

private:
    // Throws CorruptDataError if the row breaks the model's invariants.
    static models::Transaction toTransaction(const Statement& row);
    static models::TransactionDetails toDetails(const Statement& row);
    std::optional<models::Transaction> findOne(const std::string& whereClause, const SqlValue& key);
    TransferSummary transfersSince(std::int64_t walletId, const std::string& since, const char* status);

    DatabaseManager& database_;
};

} // namespace paysphere::db
