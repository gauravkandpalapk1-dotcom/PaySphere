#include "services/PaymentService.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "payments/PaymentMethod.h"
#include "services/AuthService.h"
#include "services/Clock.h"
#include "services/ReferenceGenerator.h"
#include "services/RiskEngine.h"
#include "services/UpiPinService.h"

#include <optional>

namespace paysphere::services {

namespace {

constexpr int kMaxReferenceAttempts = 3;

} // namespace

PaymentService::PaymentService(db::DatabaseManager& database, AuthService& auth, UpiPinService& pins,
                               RiskEngine& risk, ReferenceGenerator& references, const Clock& clock,
                               PaymentLimits limits)
    : database_(database),
      auth_(auth),
      pins_(pins),
      risk_(risk),
      references_(references),
      clock_(clock),
      limits_(limits),
      users_(database),
      wallets_(database),
      transactions_(database),
      idempotency_(database) {
    if (!limits_.minAmount.isPositive() || limits_.maxAmount < limits_.minAmount ||
        !limits_.recipientMaxBalance.isPositive()) {
        throw ValidationError("Payment limits must satisfy 0 < minAmount <= maxAmount");
    }
}

std::string PaymentService::newIdempotencyKey() {
    return IdempotencyLedger::newKey();
}

void PaymentService::validateAmount(payments::Money amount) const {
    if (amount < limits_.minAmount) {
        throw ValidationError("The minimum payment is " + limits_.minAmount.toString());
    }
    if (amount > limits_.maxAmount) {
        throw ValidationError("A single payment cannot exceed " + limits_.maxAmount.toString());
    }
}

void PaymentService::throwIfPinBlocked(std::int64_t userId) {
    if (const std::optional<std::string> until = pins_.blockedUntil(userId)) {
        throw PaymentBlockedError("Too many incorrect UPI PINs; payments are blocked until " + *until, *until);
    }
}

PaymentResult PaymentService::pay(const std::string& sessionToken, const payments::PaymentMethod& method,
                                  const std::string& idempotencyKey) {
    // 1. Who is paying? Nothing else is revealed to an unauthenticated caller.
    const models::User payer = auth_.authenticate(sessionToken);

    // 2. Is the request well formed?
    IdempotencyLedger::validateKey(idempotencyKey);
    const payments::Money amount = method.amount();
    validateAmount(amount);
    const std::string fingerprint = IdempotencyLedger::fingerprint(method.requestDescriptor());

    // 3. A repeat of an earlier request gets the stored result and nothing else.
    if (std::optional<models::Transaction> original = idempotency_.replay(payer.id(), idempotencyKey, fingerprint)) {
        return PaymentResult{*original, true};
    }

    if (method.payeeAddress() == payer.upiId()) {
        throw ValidationError("You cannot pay yourself");
    }
    if (!pins_.hasPin(payer.id())) {
        throw ValidationError("Set your UPI PIN before making payments");
    }
    throwIfPinBlocked(payer.id());
    const models::Wallet payerWalletAtStart = wallets_.requireByUserId(payer.id());
    if (const std::optional<std::string> until = risk_.attemptsBlockedUntil(payerWalletAtStart.id())) {
        throw PaymentBlockedError("Too many failed payment attempts; try again after " + *until, *until);
    }

    // 4. Resolve the recipient and check the PIN. Argon2id is deliberately
    //    slow, so this happens before the write lock is taken.
    const std::optional<models::User> payee = users_.findByUpiId(method.payeeAddress());
    const bool pinCorrect = payee.has_value() && method.authorize(payer.id(), pins_);

    // 5. Decide and apply atomically. The write lock (BEGIN IMMEDIATE) makes
    //    the checks, balance reads, risk statistics and writes one
    //    consistent step.
    for (int attempt = 1;; ++attempt) {
        const TimePoint now = clock_.now();
        const std::string reference = references_.next(models::TransactionType::Transfer, now);
        try {
            return database_.inTransaction([&]() -> PaymentResult {
                // Re-check under the lock: an identical request may have
                // completed since step 3...
                if (std::optional<models::Transaction> original =
                        idempotency_.replay(payer.id(), idempotencyKey, fingerprint)) {
                    return PaymentResult{*original, true};
                }
                // ...and parallel requests may have used up the PIN attempts
                // since the check in step 3. Refusing here (without revealing
                // whether this PIN was right) makes the limit exact.
                throwIfPinBlocked(payer.id());

                const models::Wallet payerWallet = wallets_.requireByUserId(payer.id());
                std::optional<models::Wallet> payeeWallet;
                if (payee) {
                    payeeWallet = wallets_.findByUserId(payee->id());
                }

                std::string failure;
                if (!payeeWallet) {
                    failure = kRecipientNotFound;
                } else if (!pinCorrect) {
                    pins_.recordAttempt(payer.id(), false);
                    failure = kIncorrectPin;
                } else {
                    pins_.recordAttempt(payer.id(), true);
                    const RiskDecision risk = risk_.assess(payerWallet.id(), payeeWallet->id(), amount);
                    if (!risk.approved) {
                        failure = kRiskDeclinedPrefix + risk.reason;
                    } else if (!payerWallet.canAfford(amount)) {
                        failure = kInsufficientFunds;
                    } else if (!payeeWallet->canReceive(amount, limits_.recipientMaxBalance)) {
                        failure = kRecipientLimit;
                    }
                }

                if (failure.empty()) {
                    // Debit, then credit. Each is a compare-and-set against
                    // the balance read above; any failure rolls back both.
                    wallets_.updateBalance(payerWallet.id(), payerWallet.balance(), payerWallet.balance() - amount);
                    wallets_.updateBalance(payeeWallet->id(), payeeWallet->balance(),
                                           payeeWallet->balance() + amount);
                }

                std::optional<std::int64_t> payeeWalletId;
                if (payeeWallet) {
                    payeeWalletId = payeeWallet->id();
                }
                const std::string createdAt = formatTimestamp(now);
                const models::Transaction recorded = transactions_.insert(models::Transaction(
                    0, reference, models::TransactionType::Transfer,
                    failure.empty() ? models::TransactionStatus::Success : models::TransactionStatus::Failed,
                    payerWallet.id(), payeeWalletId, amount, failure, createdAt, method.payeeAddress()));
                idempotency_.remember(payer.id(), idempotencyKey, fingerprint, recorded.id(), createdAt);
                return PaymentResult{recorded, false};
            });
        } catch (const db::ConstraintViolation& error) {
            // A duplicate reference, or the same key committed by a
            // concurrent request. Everything was rolled back, so try again:
            // the next attempt either gets a fresh reference or finds the replay.
            if (!error.isUniqueViolation() || attempt == kMaxReferenceAttempts) {
                throw;
            }
        }
    }
}

} // namespace paysphere::services
