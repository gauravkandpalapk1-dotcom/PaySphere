#include "database/TransactionRepository.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/Statement.h"
#include "models/Errors.h"

#include <sqlite3.h>

namespace paysphere::db {

namespace {

// Column order expected by toTransaction (0-9).
constexpr const char* kTransactionColumns =
    "id, reference, type, status, from_wallet_id, to_wallet_id, amount_paise, failure_reason, created_at, "
    "payee_upi_id";

// The same columns with a table alias, plus both parties (10-13).
constexpr const char* kDetailsSelect =
    "SELECT t.id, t.reference, t.type, t.status, t.from_wallet_id, t.to_wallet_id, t.amount_paise, "
    "       t.failure_reason, t.created_at, t.payee_upi_id, fu.full_name, fu.upi_id, tu.full_name, tu.upi_id "
    "FROM transactions t "
    "LEFT JOIN wallets fw ON fw.id = t.from_wallet_id LEFT JOIN users fu ON fu.id = fw.user_id "
    "LEFT JOIN wallets tw ON tw.id = t.to_wallet_id   LEFT JOIN users tu ON tu.id = tw.user_id ";

// ?1 wallet id, ?2 direction ('SENT', 'RECEIVED' or NULL), ?3 status or NULL.
// The first condition is Transaction::isVisibleTo expressed in SQL.
constexpr const char* kHistoryWhere =
    "WHERE (t.from_wallet_id = ?1 "
    "       OR (t.to_wallet_id = ?1 AND (t.type = 'DEPOSIT' OR t.status = 'SUCCESS'))) "
    "  AND (?2 IS NULL OR (?2 = 'SENT' AND t.from_wallet_id = ?1) "
    "                  OR (?2 = 'RECEIVED' AND t.to_wallet_id = ?1)) "
    "  AND (?3 IS NULL OR t.status = ?3) ";

std::optional<std::int64_t> optionalId(const Statement& row, int column) {
    if (row.columnIsNull(column)) {
        return std::nullopt;
    }
    return row.columnInt64(column);
}

std::string optionalText(const Statement& row, int column) {
    return row.columnIsNull(column) ? std::string() : row.columnText(column);
}

SqlValue nullable(const std::optional<std::int64_t>& id) {
    return id ? SqlValue(*id) : SqlValue(nullptr);
}

SqlValue nullable(const std::string& text) {
    return text.empty() ? SqlValue(nullptr) : SqlValue(text);
}

std::optional<models::Party> partyAt(const Statement& row, int column) {
    if (row.columnIsNull(column)) {
        return std::nullopt;
    }
    return models::Party{row.columnText(column), row.columnText(column + 1)};
}

SqlParams historyParams(std::int64_t walletId, const HistoryFilter& filter) {
    SqlValue direction = nullptr;
    if (filter.direction) {
        direction = std::string(*filter.direction == models::TransactionDirection::Sent ? "SENT" : "RECEIVED");
    }
    SqlValue status = nullptr;
    if (filter.status) {
        status = std::string(models::toString(*filter.status));
    }
    return {walletId, direction, status};
}

} // namespace

TransactionRepository::TransactionRepository(DatabaseManager& database) : database_(database) {}

models::Transaction TransactionRepository::toTransaction(const Statement& row) {
    try {
        return models::Transaction(row.columnInt64(0),
                                   row.columnText(1),
                                   models::parseTransactionType(row.columnText(2)),
                                   models::parseTransactionStatus(row.columnText(3)),
                                   optionalId(row, 4),
                                   optionalId(row, 5),
                                   payments::Money::fromPaise(row.columnInt64(6)),
                                   optionalText(row, 7),
                                   row.columnText(8),
                                   optionalText(row, 9));
    } catch (const ValidationError& error) {
        // A stored row the model rejects is corruption, not bad user input.
        throw CorruptDataError(std::string("Stored transaction row is invalid: ") + error.what());
    }
}

models::TransactionDetails TransactionRepository::toDetails(const Statement& row) {
    return models::TransactionDetails{toTransaction(row), partyAt(row, 10), partyAt(row, 12)};
}

models::Transaction TransactionRepository::insert(const models::Transaction& transaction) {
    if (transaction.isPersisted()) {
        throw DatabaseError("Cannot insert a transaction that already has an id", SQLITE_MISUSE);
    }
    const std::int64_t id = database_.insert(
        "INSERT INTO transactions (reference, type, status, from_wallet_id, to_wallet_id, "
        "amount_paise, failure_reason, created_at, payee_upi_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, coalesce(?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now')), ?)",
        {transaction.reference(),
         models::toString(transaction.type()),
         models::toString(transaction.status()),
         nullable(transaction.fromWalletId()),
         nullable(transaction.toWalletId()),
         transaction.amount().paise(),
         nullable(transaction.failureReason()),
         nullable(transaction.createdAt()),
         nullable(transaction.payeeUpiId())});

    std::optional<models::Transaction> stored = findById(id);
    if (!stored) {
        throw DatabaseError("Inserted transaction could not be read back", SQLITE_INTERNAL);
    }
    return *stored;
}

std::optional<models::Transaction> TransactionRepository::findOne(const std::string& whereClause,
                                                                  const SqlValue& key) {
    Statement query = database_.prepare(std::string("SELECT ") + kTransactionColumns +
                                            " FROM transactions WHERE " + whereClause,
                                        {key});
    if (!query.step()) {
        return std::nullopt;
    }
    return toTransaction(query);
}

std::optional<models::Transaction> TransactionRepository::findById(std::int64_t id) {
    return findOne("id = ?", id);
}

std::optional<models::Transaction> TransactionRepository::findByReference(const std::string& reference) {
    return findOne("reference = ?", reference);
}

TransactionRepository::TransferSummary TransactionRepository::transfersSince(std::int64_t walletId,
                                                                              const std::string& since,
                                                                              const char* status) {
    Statement query = database_.prepare(
        "SELECT count(*), coalesce(sum(amount_paise), 0), min(created_at) FROM transactions "
        "WHERE from_wallet_id = ? AND type = 'TRANSFER' AND status = ? AND created_at > ?",
        {walletId, std::string(status), since});
    TransferSummary summary;
    if (query.step()) {
        summary.count = query.columnInt64(0);
        summary.total = payments::Money::fromPaise(query.columnInt64(1));
        if (!query.columnIsNull(2)) {
            summary.oldestAt = query.columnText(2);
        }
    }
    return summary;
}

TransactionRepository::TransferSummary TransactionRepository::successfulTransfersSince(std::int64_t walletId,
                                                                                        const std::string& since) {
    return transfersSince(walletId, since, "SUCCESS");
}

TransactionRepository::TransferSummary TransactionRepository::failedTransfersSince(std::int64_t walletId,
                                                                                    const std::string& since) {
    return transfersSince(walletId, since, "FAILED");
}

std::optional<std::string> TransactionRepository::firstSuccessfulTransferBetween(std::int64_t fromWalletId,
                                                                                 std::int64_t toWalletId) {
    return database_.queryText(
        "SELECT min(created_at) FROM transactions WHERE from_wallet_id = ? AND to_wallet_id = ? "
        "AND type = 'TRANSFER' AND status = 'SUCCESS'",
        {fromWalletId, toWalletId});
}

std::vector<models::TransactionDetails> TransactionRepository::history(std::int64_t walletId,
                                                                       const HistoryFilter& filter) {
    SqlParams params = historyParams(walletId, filter);
    params.emplace_back(filter.limit);
    params.emplace_back(filter.offset);
    std::vector<models::TransactionDetails> page;
    database_.query(std::string(kDetailsSelect) + kHistoryWhere + "ORDER BY t.id DESC LIMIT ?4 OFFSET ?5",
                    params, [&page](const Statement& row) { page.push_back(toDetails(row)); });
    return page;
}

std::int64_t TransactionRepository::countHistory(std::int64_t walletId, const HistoryFilter& filter) {
    return database_
        .queryInt64(std::string("SELECT count(*) FROM transactions t ") + kHistoryWhere,
                    historyParams(walletId, filter))
        .value_or(0);
}

std::optional<models::TransactionDetails> TransactionRepository::findDetailsByReference(
    const std::string& reference) {
    Statement query = database_.prepare(std::string(kDetailsSelect) + "WHERE t.reference = ?", {reference});
    if (!query.step()) {
        return std::nullopt;
    }
    return toDetails(query);
}

payments::Money TransactionRepository::ledgerBalance(std::int64_t walletId) {
    const std::optional<std::int64_t> paise = database_.queryInt64(
        // sum() over INTEGER values stays an exact integer (and raises an
        // error on overflow); total() would return a floating-point value.
        "SELECT coalesce(sum(CASE WHEN to_wallet_id = ?1 THEN amount_paise ELSE 0 END), 0) "
        "     - coalesce(sum(CASE WHEN from_wallet_id = ?1 THEN amount_paise ELSE 0 END), 0) "
        "FROM transactions "
        "WHERE status = 'SUCCESS' AND (to_wallet_id = ?1 OR from_wallet_id = ?1)",
        {walletId});
    return payments::Money::fromPaise(paise.value_or(0));
}

} // namespace paysphere::db
