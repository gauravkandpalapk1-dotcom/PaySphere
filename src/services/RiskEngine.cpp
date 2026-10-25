#include "services/RiskEngine.h"

#include "models/Errors.h"
#include "services/Clock.h"

namespace paysphere::services {

RiskRule::~RiskRule() = default;

VelocityRule::VelocityRule(std::int64_t maxTransfers) : maxTransfers_(maxTransfers) {
    if (maxTransfers_ < 1) {
        throw ValidationError("Velocity limit must be at least 1");
    }
}

std::optional<std::string> VelocityRule::check(const RiskContext& context) const {
    if (context.recentTransfers >= maxTransfers_) {
        return std::string("Too many payments in a short time; please try again later");
    }
    return std::nullopt;
}

DailyLimitRule::DailyLimitRule(payments::Money limit) : limit_(limit) {
    if (!limit_.isPositive()) {
        throw ValidationError("Daily limit must be positive");
    }
}

std::optional<std::string> DailyLimitRule::check(const RiskContext& context) const {
    // Written as a comparison against the remaining allowance, so no sum can overflow.
    if (context.outgoingLast24Hours >= limit_ ||
        context.amount > payments::Money::fromPaise(limit_.paise() - context.outgoingLast24Hours.paise())) {
        return "This payment would exceed your 24-hour limit of " + limit_.toString();
    }
    return std::nullopt;
}

NewPayeeRule::NewPayeeRule(payments::Money limit, std::chrono::seconds coolingOffPeriod)
    : limit_(limit), coolingOffPeriod_(coolingOffPeriod) {
    if (!limit_.isPositive() || coolingOffPeriod_.count() < 0) {
        throw ValidationError("New-payee limit must be positive and the cooling-off period non-negative");
    }
}

std::optional<std::string> NewPayeeRule::check(const RiskContext& context) const {
    const bool newPayee =
        !context.firstPaidPayeeAt || context.now - *context.firstPaidPayeeAt < coolingOffPeriod_;
    if (newPayee && context.amount > limit_) {
        const auto hours = std::chrono::duration_cast<std::chrono::hours>(coolingOffPeriod_).count();
        return "Payments to a new recipient are limited to " + limit_.toString() + " until " +
               std::to_string(hours) + " hours after your first payment to them";
    }
    return std::nullopt;
}

RiskEngine::RiskEngine(db::DatabaseManager& database, const Clock& clock, RiskLimits limits)
    : clock_(clock), limits_(limits), transactions_(database) {
    if (limits_.velocityWindow.count() <= 0 || limits_.maxFailedAttemptsPerWindow < 1) {
        throw ValidationError("Velocity window and failed-attempt limit must be positive");
    }
    addRule(std::make_unique<VelocityRule>(limits_.maxTransfersPerWindow));
    addRule(std::make_unique<DailyLimitRule>(limits_.dailyLimit));
    addRule(std::make_unique<NewPayeeRule>(limits_.newPayeeLimit, limits_.newPayeeCoolingOff));
}

void RiskEngine::addRule(std::unique_ptr<RiskRule> rule) {
    if (!rule) {
        throw ValidationError("A risk rule must not be null");
    }
    rules_.push_back(std::move(rule));
}

RiskDecision RiskEngine::assess(std::int64_t payerWalletId, std::int64_t payeeWalletId,
                                payments::Money amount) {
    const TimePoint now = clock_.now();
    RiskContext context;
    context.payerWalletId = payerWalletId;
    context.payeeWalletId = payeeWalletId;
    context.amount = amount;
    context.now = now;
    context.recentTransfers =
        transactions_.successfulTransfersSince(payerWalletId, formatTimestamp(now - limits_.velocityWindow)).count;
    context.outgoingLast24Hours =
        transactions_.successfulTransfersSince(payerWalletId, formatTimestamp(now - std::chrono::hours(24))).total;
    if (const auto firstPaid = transactions_.firstSuccessfulTransferBetween(payerWalletId, payeeWalletId)) {
        context.firstPaidPayeeAt = parseTimestamp(*firstPaid);
    }

    for (const auto& rule : rules_) {
        if (std::optional<std::string> reason = rule->check(context)) {
            return RiskDecision{false, rule->name(), *reason};
        }
    }
    return RiskDecision{};
}

std::optional<std::string> RiskEngine::attemptsBlockedUntil(std::int64_t payerWalletId) {
    const TimePoint now = clock_.now();
    const auto failed =
        transactions_.failedTransfersSince(payerWalletId, formatTimestamp(now - limits_.velocityWindow));
    if (failed.count < limits_.maxFailedAttemptsPerWindow || !failed.oldestAt) {
        return std::nullopt;
    }
    // Attempts resume once the oldest counted failure leaves the window.
    return formatTimestamp(parseTimestamp(*failed.oldestAt) + limits_.velocityWindow);
}

} // namespace paysphere::services
