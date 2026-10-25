#include "database/WalletRepository.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/Statement.h"
#include "models/Errors.h"

#include <sqlite3.h>

#include <string>

namespace paysphere::db {

namespace {

constexpr const char* kWalletColumns = "id, user_id, balance_paise, created_at";

} // namespace

WalletRepository::WalletRepository(DatabaseManager& database) : database_(database) {}

models::Wallet WalletRepository::toWallet(const Statement& row) {
    try {
        return models::Wallet(row.columnInt64(0), row.columnInt64(1),
                              payments::Money::fromPaise(row.columnInt64(2)), row.columnText(3));
    } catch (const ValidationError& error) {
        throw CorruptDataError(std::string("Stored wallet row is invalid: ") + error.what());
    }
}

models::Wallet WalletRepository::createForUser(std::int64_t userId, const std::string& createdAt) {
    database_.insert("INSERT INTO wallets (user_id, balance_paise, created_at) "
                     "VALUES (?, 0, coalesce(?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now')))",
                     {userId, createdAt.empty() ? SqlValue(nullptr) : SqlValue(createdAt)});
    std::optional<models::Wallet> wallet = findByUserId(userId);
    if (!wallet) {
        throw DatabaseError("Created wallet could not be read back", SQLITE_INTERNAL);
    }
    return *wallet;
}

std::optional<models::Wallet> WalletRepository::findById(std::int64_t walletId) {
    Statement query = database_.prepare(
        std::string("SELECT ") + kWalletColumns + " FROM wallets WHERE id = ?", {walletId});
    if (!query.step()) {
        return std::nullopt;
    }
    return toWallet(query);
}

models::Wallet WalletRepository::requireByUserId(std::int64_t userId) {
    std::optional<models::Wallet> wallet = findByUserId(userId);
    if (!wallet) {
        throw CorruptDataError("No wallet exists for user " + std::to_string(userId));
    }
    return *wallet;
}

std::optional<models::Wallet> WalletRepository::findByUserId(std::int64_t userId) {
    Statement query = database_.prepare(
        std::string("SELECT ") + kWalletColumns + " FROM wallets WHERE user_id = ?", {userId});
    if (!query.step()) {
        return std::nullopt;
    }
    return toWallet(query);
}

void WalletRepository::updateBalance(std::int64_t walletId, payments::Money expectedBalance,
                                     payments::Money newBalance) {
    const int changed = database_.execute(
        "UPDATE wallets SET balance_paise = ? WHERE id = ? AND balance_paise = ?",
        {newBalance.paise(), walletId, expectedBalance.paise()});
    if (changed != 1) {
        throw ConcurrentModificationError("Wallet " + std::to_string(walletId) +
                                          " was modified concurrently or does not exist; nothing was changed");
    }
}

} // namespace paysphere::db
