#include "models/Transaction.h"

#include "models/Errors.h"

#include <stdexcept>
#include <utility>

namespace paysphere::models {

const char* toString(TransactionType type) {
    switch (type) {
    case TransactionType::Deposit:
        return "DEPOSIT";
    case TransactionType::Transfer:
        return "TRANSFER";
    }
    throw std::logic_error("Unhandled TransactionType");
}

const char* toString(TransactionStatus status) {
    switch (status) {
    case TransactionStatus::Success:
        return "SUCCESS";
    case TransactionStatus::Failed:
        return "FAILED";
    }
    throw std::logic_error("Unhandled TransactionStatus");
}

TransactionType parseTransactionType(const std::string& text) {
    if (text == "DEPOSIT") {
        return TransactionType::Deposit;
    }
    if (text == "TRANSFER") {
        return TransactionType::Transfer;
    }
    throw ValidationError("Unknown transaction type: " + text);
}

TransactionStatus parseTransactionStatus(const std::string& text) {
    if (text == "SUCCESS") {
        return TransactionStatus::Success;
    }
    if (text == "FAILED") {
        return TransactionStatus::Failed;
    }
    throw ValidationError("Unknown transaction status: " + text);
}

Transaction::Transaction(std::int64_t id,
                         std::string reference,
                         TransactionType type,
                         TransactionStatus status,
                         std::optional<std::int64_t> fromWalletId,
                         std::optional<std::int64_t> toWalletId,
                         payments::Money amount,
                         std::string failureReason,
                         std::string createdAt,
                         std::string payeeUpiId)
    : id_(id),
      reference_(std::move(reference)),
      type_(type),
      status_(status),
      fromWalletId_(fromWalletId),
      toWalletId_(toWalletId),
      amount_(amount),
      failureReason_(std::move(failureReason)),
      createdAt_(std::move(createdAt)),
      payeeUpiId_(std::move(payeeUpiId)) {
    validate();
}

bool Transaction::involves(std::int64_t walletId) const noexcept {
    return fromWalletId_ == walletId || toWalletId_ == walletId;
}

bool Transaction::isVisibleTo(std::int64_t walletId) const noexcept {
    if (fromWalletId_ == walletId) {
        return true;
    }
    return toWalletId_ == walletId && (type_ == TransactionType::Deposit || succeeded());
}

TransactionDirection Transaction::directionFor(std::int64_t walletId) const {
    if (fromWalletId_ == walletId) {
        return TransactionDirection::Sent;
    }
    if (toWalletId_ == walletId) {
        return TransactionDirection::Received;
    }
    throw ValidationError("Wallet " + std::to_string(walletId) + " is not part of this transaction");
}

void Transaction::validate() const {
    if (id_ < 0) {
        throw ValidationError("Transaction id must not be negative");
    }
    if (reference_.empty()) {
        throw ValidationError("Transaction reference is required");
    }
    if (!amount_.isPositive()) {
        throw ValidationError("Transaction amount must be positive");
    }
    if ((fromWalletId_ && *fromWalletId_ <= 0) || (toWalletId_ && *toWalletId_ <= 0)) {
        throw ValidationError("Transaction wallet ids must be positive");
    }

    switch (type_) {
    case TransactionType::Deposit:
        if (fromWalletId_ || !toWalletId_) {
            throw ValidationError("A deposit needs a destination wallet and no source wallet");
        }
        if (!payeeUpiId_.empty()) {
            throw ValidationError("A deposit has no payee UPI ID");
        }
        break;
    case TransactionType::Transfer:
        if (!fromWalletId_) {
            throw ValidationError("A transfer needs a source wallet");
        }
        if (!toWalletId_ && status_ != TransactionStatus::Failed) {
            throw ValidationError("A successful transfer needs a destination wallet");
        }
        if (toWalletId_ && *toWalletId_ == *fromWalletId_) {
            throw ValidationError("A transfer cannot go to the source wallet");
        }
        break;
    }

    const bool failed = status_ == TransactionStatus::Failed;
    if (failed && failureReason_.empty()) {
        throw ValidationError("A failed transaction needs a failure reason");
    }
    if (!failed && !failureReason_.empty()) {
        throw ValidationError("A successful transaction cannot have a failure reason");
    }
}

} // namespace paysphere::models
