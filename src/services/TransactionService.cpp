#include "services/TransactionService.h"

#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "services/AuthService.h"

#include <algorithm>
#include <utility>

namespace paysphere::services {

namespace {

// Transaction IDs are uppercase letters and digits; accept what a user might
// type or paste ("  dep2026...  ").
std::string normalizeReference(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    std::string reference = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(reference.begin(), reference.end(), reference.begin(),
                   [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    return reference;
}

} // namespace

TransactionService::TransactionService(db::DatabaseManager& database, AuthService& auth)
    : database_(database), auth_(auth), wallets_(database), transactions_(database) {}

models::Wallet TransactionService::callerWallet(const std::string& sessionToken) {
    return wallets_.requireByUserId(auth_.authenticate(sessionToken).id());
}

HistoryEntry TransactionService::toEntry(models::TransactionDetails details, std::int64_t walletId) {
    const models::TransactionDirection direction = details.transaction.directionFor(walletId);
    std::optional<models::Party> counterparty =
        direction == models::TransactionDirection::Sent ? details.to : details.from;
    return HistoryEntry{std::move(details.transaction), direction, std::move(counterparty)};
}

HistoryPage TransactionService::history(const std::string& sessionToken, const HistoryQuery& query) {
    const models::Wallet wallet = callerWallet(sessionToken);
    if (query.limit < 1 || query.limit > kMaxPageSize) {
        throw ValidationError("Page size must be between 1 and " + std::to_string(kMaxPageSize));
    }
    if (query.offset < 0) {
        throw ValidationError("Page offset must not be negative");
    }

    const db::HistoryFilter filter{query.direction, query.status, query.limit, query.offset};
    HistoryPage page;
    page.totalMatching = transactions_.countHistory(wallet.id(), filter);
    for (models::TransactionDetails& details : transactions_.history(wallet.id(), filter)) {
        // Defence in depth: the SQL already applies this rule; check it again in C++.
        if (!details.transaction.isVisibleTo(wallet.id())) {
            throw PaySphereError("History query returned a transaction the user may not see");
        }
        page.entries.push_back(toEntry(std::move(details), wallet.id()));
    }
    return page;
}

std::optional<models::TransactionDetails> TransactionService::visibleDetails(std::int64_t walletId,
                                                                             const std::string& reference) {
    const std::string normalized = normalizeReference(reference);
    if (normalized.empty() || normalized.size() > 64) {
        return std::nullopt;
    }
    std::optional<models::TransactionDetails> details = transactions_.findDetailsByReference(normalized);
    if (!details || !details->transaction.isVisibleTo(walletId)) {
        return std::nullopt;
    }
    return details;
}

std::optional<HistoryEntry> TransactionService::findByReference(const std::string& sessionToken,
                                                                const std::string& reference) {
    const models::Wallet wallet = callerWallet(sessionToken);
    std::optional<models::TransactionDetails> details = visibleDetails(wallet.id(), reference);
    if (!details) {
        return std::nullopt;
    }
    return toEntry(std::move(*details), wallet.id());
}

Receipt TransactionService::receipt(const std::string& sessionToken, const std::string& reference) {
    const models::Wallet wallet = callerWallet(sessionToken);
    std::optional<models::TransactionDetails> details = visibleDetails(wallet.id(), reference);
    if (!details) {
        throw AuthorizationError("No transaction with this ID was found in your account");
    }
    const models::TransactionDirection direction = details->transaction.directionFor(wallet.id());
    return Receipt(std::move(*details), direction);
}

LedgerAudit TransactionService::audit(const std::string& sessionToken) {
    const models::User user = auth_.authenticate(sessionToken);
    // Both reads in one snapshot: a payment landing between them would
    // otherwise show up as a false mismatch. A read transaction does not
    // take the write lock, so auditing never blocks payments.
    return database_.inReadTransaction([&] {
        const models::Wallet wallet = wallets_.requireByUserId(user.id());
        return LedgerAudit{wallet.balance(), transactions_.ledgerBalance(wallet.id())};
    });
}

} // namespace paysphere::services
