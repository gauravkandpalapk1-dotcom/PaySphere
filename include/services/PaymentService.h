#pragma once

#include "database/TransactionRepository.h"
#include "database/UserRepository.h"
#include "database/WalletRepository.h"
#include "models/Transaction.h"
#include "payments/Money.h"
#include "services/IdempotencyLedger.h"
#include "services/WalletLimits.h"

#include <string>

namespace paysphere::payments {
class PaymentMethod;
}

namespace paysphere::services {

class AuthService;
class Clock;
class ReferenceGenerator;
class RiskEngine;
class UpiPinService;

struct PaymentLimits {
    payments::Money minAmount = payments::Money::fromPaise(1'00);        // Rs. 1
    payments::Money maxAmount = payments::Money::fromPaise(1'00'000'00); // Rs. 1,00,000 per payment
    // Incoming money is also capped, like deposits.
    payments::Money recipientMaxBalance = WalletLimits{}.maxBalance;
};

struct PaymentResult {
    models::Transaction transaction; // SUCCESS or FAILED; failureReason() explains a failure
    bool replayed = false;           // true if this is the stored result of an earlier identical request
};

// Simulated transfers between registered users. No real bank or UPI network
// is contacted.
//
// Outcomes:
//   - Thrown and not recorded: SessionError (not logged in), ValidationError
//     (malformed input, paying yourself, no PIN set), PaymentBlockedError
//     (too many wrong PINs, or too many failed attempts recently),
//     IdempotencyConflictError (key reused for a different request).
//   - Recorded as FAILED and returned: unknown recipient, incorrect PIN,
//     risk decline, insufficient balance, recipient's wallet full.
//   - Recorded as SUCCESS and returned: the sender is debited, the receiver
//     credited and the transaction recorded, all in one database
//     transaction.
//
// Idempotency (see IdempotencyLedger): a repeat with the same key and the
// same request (payee and amount) returns the stored result, success or
// failure, without moving money again. To try again after a failure, send a
// new key.
class PaymentService {
public:
    static constexpr const char* kRecipientNotFound = "Recipient UPI ID not found";
    static constexpr const char* kIncorrectPin = "Incorrect UPI PIN";
    static constexpr const char* kInsufficientFunds = "Insufficient balance";
    static constexpr const char* kRecipientLimit = "The recipient's wallet cannot receive this amount";
    static constexpr const char* kRiskDeclinedPrefix = "Declined by risk check: ";

    PaymentService(db::DatabaseManager& database, AuthService& auth, UpiPinService& pins, RiskEngine& risk,
                   ReferenceGenerator& references, const Clock& clock, PaymentLimits limits = {});

    // idempotencyKey: 8-128 characters of letters, digits, '-' or '_',
    // unique per payment attempt (newIdempotencyKey() makes one).
    PaymentResult pay(const std::string& sessionToken, const payments::PaymentMethod& method,
                      const std::string& idempotencyKey);

    // A fresh random key (32 hex characters) for clients that need one.
    static std::string newIdempotencyKey();

    const PaymentLimits& limits() const noexcept { return limits_; }

private:
    void validateAmount(payments::Money amount) const;
    void throwIfPinBlocked(std::int64_t userId);

    db::DatabaseManager& database_;
    AuthService& auth_;
    UpiPinService& pins_;
    RiskEngine& risk_;
    ReferenceGenerator& references_;
    const Clock& clock_;
    PaymentLimits limits_;
    db::UserRepository users_;
    db::WalletRepository wallets_;
    db::TransactionRepository transactions_;
    IdempotencyLedger idempotency_;
};

} // namespace paysphere::services
