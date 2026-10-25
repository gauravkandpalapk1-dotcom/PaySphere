#include "services/Receipt.h"

#include "services/Clock.h"

#include <sstream>
#include <utility>

namespace paysphere::services {

namespace {

std::string describe(const std::optional<models::Party>& party, const std::string& fallback) {
    if (!party) {
        return fallback;
    }
    return party->fullName + " <" + party->upiId + ">";
}

} // namespace

Receipt::Receipt(models::TransactionDetails details, models::TransactionDirection viewerDirection)
    : details_(std::move(details)), viewerDirection_(viewerDirection) {}

std::string Receipt::toText() const {
    const models::Transaction& t = details_.transaction;
    const bool deposit = t.type() == models::TransactionType::Deposit;
    const char* rule = "==================================================\n";

    std::string type;
    if (deposit) {
        type = "Wallet deposit";
    } else {
        type = viewerDirection_ == models::TransactionDirection::Sent ? "UPI transfer (sent)"
                                                                      : "UPI transfer (received)";
    }

    std::ostringstream out;
    out << rule
        << "                PaySphere receipt\n"
        << "   Simulated transaction - no real money moved\n"
        << rule
        << "Transaction ID  " << t.reference() << '\n'
        << "Type            " << type << '\n'
        << "Status          " << models::toString(t.status()) << '\n'
        << "Amount          " << t.amount().toString() << '\n'
        << "From            " << describe(details_.from, deposit ? "Simulated bank deposit" : "-") << '\n'
        << "To              "
        << describe(details_.to, t.payeeUpiId().empty() ? std::string("(recipient not found)")
                                                         : "(not found) " + t.payeeUpiId())
        << '\n'
        << "Date            " << formatIndianTime(parseTimestamp(t.createdAt())) << '\n'
        << "UTC             " << t.createdAt() << '\n';
    if (!t.succeeded()) {
        out << "Reason          " << t.failureReason() << '\n';
    }
    out << rule;
    return out.str();
}

} // namespace paysphere::services
