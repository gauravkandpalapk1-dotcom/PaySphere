#pragma once

#include "payments/Money.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::models {

enum class TransactionType { Deposit, Transfer };
enum class TransactionStatus { Success, Failed };
// Relative to one wallet: money out (Sent) or money in (Received, which
// includes deposits).
enum class TransactionDirection { Sent, Received };

// Database spellings: "DEPOSIT", "TRANSFER", "SUCCESS", "FAILED".
const char* toString(TransactionType type);
const char* toString(TransactionStatus status);
// Throw ValidationError for unknown text.
TransactionType parseTransactionType(const std::string& text);
TransactionStatus parseTransactionStatus(const std::string& text);

// An immutable record of one money movement (one row of the transactions table).
//
// Invariants, mirrored by CHECK constraints in the schema:
//   - the amount is positive;
//   - a deposit has a destination wallet and no source wallet;
//   - a transfer has a source wallet, and a destination wallet different from
//     the source unless it failed before a recipient was resolved;
//   - a failure reason is present exactly when the status is Failed.
class Transaction {
public:
    Transaction(std::int64_t id,
                std::string reference,
                TransactionType type,
                TransactionStatus status,
                std::optional<std::int64_t> fromWalletId,
                std::optional<std::int64_t> toWalletId,
                payments::Money amount,
                std::string failureReason = {},
                std::string createdAt = {},
                std::string payeeUpiId = {});

    std::int64_t id() const noexcept { return id_; }
    bool isPersisted() const noexcept { return id_ > 0; }
    // Public transaction ID shown to users, e.g. on a receipt.
    const std::string& reference() const noexcept { return reference_; }
    TransactionType type() const noexcept { return type_; }
    TransactionStatus status() const noexcept { return status_; }
    bool succeeded() const noexcept { return status_ == TransactionStatus::Success; }
    const std::optional<std::int64_t>& fromWalletId() const noexcept { return fromWalletId_; }
    const std::optional<std::int64_t>& toWalletId() const noexcept { return toWalletId_; }
    payments::Money amount() const noexcept { return amount_; }
    const std::string& failureReason() const noexcept { return failureReason_; }
    const std::string& createdAt() const noexcept { return createdAt_; }
    // For transfers: the UPI ID the payer entered, kept even when no such
    // recipient existed, so the audit trail shows who was meant to be paid.
    // Empty for deposits and for transfers recorded before schema v6.
    const std::string& payeeUpiId() const noexcept { return payeeUpiId_; }

    // True if the wallet is the source or the destination.
    bool involves(std::int64_t walletId) const noexcept;
    // Who may see this record:
    //   - the sender always;
    //   - the owner of a deposit always (it is their own action);
    //   - the receiver of a transfer only if it succeeded. A failed attempt
    //     to pay someone (wrong PIN, no funds) stays private to the payer.
    bool isVisibleTo(std::int64_t walletId) const noexcept;
    // Sent if walletId is the source, otherwise Received.
    // Throws ValidationError if the wallet is not involved.
    TransactionDirection directionFor(std::int64_t walletId) const;

private:
    void validate() const;

    std::int64_t id_;
    std::string reference_;
    TransactionType type_;
    TransactionStatus status_;
    std::optional<std::int64_t> fromWalletId_;
    std::optional<std::int64_t> toWalletId_;
    payments::Money amount_;
    std::string failureReason_;
    std::string createdAt_;
    std::string payeeUpiId_;
};

} // namespace paysphere::models
