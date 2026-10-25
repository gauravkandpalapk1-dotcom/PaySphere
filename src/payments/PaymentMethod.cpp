#include "payments/PaymentMethod.h"

#include "models/Errors.h"

namespace paysphere::payments {

CredentialVerifier::~CredentialVerifier() = default;

PaymentMethod::PaymentMethod(Money amount) : amount_(amount) {
    if (!amount_.isPositive()) {
        throw ValidationError("Payment amount must be greater than zero");
    }
}

PaymentMethod::~PaymentMethod() = default;

std::string PaymentMethod::requestDescriptor() const {
    return std::string(methodName()) + '\n' + payeeAddress() + '\n' + std::to_string(amount_.paise());
}

} // namespace paysphere::payments
