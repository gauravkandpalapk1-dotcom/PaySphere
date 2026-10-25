#pragma once

#include "database/TransactionRepository.h"
#include "payments/Money.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paysphere::services {

class Clock;

// Facts about a proposed transfer, gathered once by the RiskEngine and
// shared by every rule.
struct RiskContext {
    std::int64_t payerWalletId = 0;
    std::int64_t payeeWalletId = 0;
    payments::Money amount;
    std::chrono::system_clock::time_point now;
    std::int64_t recentTransfers = 0;       // successful, within the velocity window
    payments::Money outgoingLast24Hours;    // successful transfers sent
    // When the payer first successfully paid this payee (nullopt = never).
    std::optional<std::chrono::system_clock::time_point> firstPaidPayeeAt;
};

// One fraud or limit check (Strategy pattern). The engine runs every rule
// through this interface, so new checks can be added without changing the
// engine (open/closed principle).
class RiskRule {
public:
    virtual ~RiskRule();
    virtual const char* name() const noexcept = 0;
    // A user-facing reason if the payment must be declined, nullopt if fine.
    virtual std::optional<std::string> check(const RiskContext& context) const = 0;
};

// Too many transfers in a short time: a sign of a compromised account or a
// script.
class VelocityRule final : public RiskRule {
public:
    explicit VelocityRule(std::int64_t maxTransfers);
    const char* name() const noexcept override { return "velocity"; }
    std::optional<std::string> check(const RiskContext& context) const override;

private:
    std::int64_t maxTransfers_;
};

// Caps the total sent in any rolling 24 hours.
class DailyLimitRule final : public RiskRule {
public:
    explicit DailyLimitRule(payments::Money limit);
    const char* name() const noexcept override { return "daily-limit"; }
    std::optional<std::string> check(const RiskContext& context) const override;

private:
    payments::Money limit_;
};

// Caps payments to someone the payer started paying recently (or never
// paid), limiting losses from scams that trick users into paying a stranger.
// The cap lasts for a cooling-off period after the *first* payment, so a
// token Rs. 1 "verification" payment does not lift it.
class NewPayeeRule final : public RiskRule {
public:
    NewPayeeRule(payments::Money limit, std::chrono::seconds coolingOffPeriod);
    const char* name() const noexcept override { return "new-payee"; }
    std::optional<std::string> check(const RiskContext& context) const override;

private:
    payments::Money limit_;
    std::chrono::seconds coolingOffPeriod_;
};

struct RiskLimits {
    std::int64_t maxTransfersPerWindow = 5;
    std::chrono::seconds velocityWindow{std::chrono::minutes(10)};
    payments::Money dailyLimit = payments::Money::fromPaise(1'00'000'00); // Rs. 1,00,000
    payments::Money newPayeeLimit = payments::Money::fromPaise(20'000'00); // Rs. 20,000
    std::chrono::seconds newPayeeCoolingOff{std::chrono::hours(24)};
    // Failed transfers (any reason) allowed per velocity window before
    // further attempts are refused outright. Bounds probing of UPI IDs and
    // the audit-log growth it causes.
    std::int64_t maxFailedAttemptsPerWindow = 10;
};

struct RiskDecision {
    bool approved = true;
    std::string ruleName; // the rule that declined, if any
    std::string reason;
};

// Runs every risk rule against a proposed transfer. The first rule that
// objects declines the payment.
class RiskEngine {
public:
    // Installs the three default rules configured by `limits`.
    RiskEngine(db::DatabaseManager& database, const Clock& clock, RiskLimits limits = {});

    void addRule(std::unique_ptr<RiskRule> rule);

    // Call inside the payment's database transaction, so the statistics
    // cannot change before the payment is recorded.
    RiskDecision assess(std::int64_t payerWalletId, std::int64_t payeeWalletId, payments::Money amount);

    // If the payer has had too many failed transfers recently, the time
    // (UTC timestamp) until which new attempts should be refused.
    std::optional<std::string> attemptsBlockedUntil(std::int64_t payerWalletId);

    const RiskLimits& limits() const noexcept { return limits_; }

private:
    const Clock& clock_;
    RiskLimits limits_;
    db::TransactionRepository transactions_;
    std::vector<std::unique_ptr<RiskRule>> rules_;
};

} // namespace paysphere::services
