#include "payments/UPIPayment.h"

#include "models/Errors.h"

#include <algorithm>
#include <utility>

namespace paysphere::payments {

UPIPayment::UPIPayment(const std::string& payeeUpiId, Money amount, std::string upiPin)
    : PaymentMethod(amount), pin_(std::move(upiPin)), payee_(payeeUpiId) {
    validatePinFormat(pin_.value());
}

bool UPIPayment::authorize(std::int64_t payerUserId, CredentialVerifier& verifier) const {
    return verifier.verifyCredential(payerUserId, pin_.value());
}

void UPIPayment::validatePinFormat(const std::string& pin) {
    const bool digitsOnly = std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
    if ((pin.size() != 4 && pin.size() != 6) || !digitsOnly) {
        throw ValidationError("A UPI PIN is 4 or 6 digits");
    }
}

} // namespace paysphere::payments
