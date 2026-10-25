#pragma once

#include "payments/PaymentMethod.h"
#include "payments/SecretString.h"
#include "payments/UpiId.h"

#include <cstdint>
#include <string>

namespace paysphere::payments {

// A simulated UPI payment: pay `amount` to a UPI ID, authorized by the
// payer's UPI PIN. Construction validates the UPI ID, the amount and the PIN
// format; whether the PIN is *correct* is decided later by authorize().
class UPIPayment final : public PaymentMethod {
public:
    // Throws ValidationError for a malformed UPI ID, a non-positive amount or
    // a PIN that is not 4 or 6 digits. The PIN is wiped from memory when
    // this object is destroyed, even if construction fails.
    UPIPayment(const std::string& payeeUpiId, Money amount, std::string upiPin);

    const char* methodName() const noexcept override { return "UPI"; }
    const std::string& payeeAddress() const noexcept override { return payee_.value(); }
    bool authorize(std::int64_t payerUserId, CredentialVerifier& verifier) const override;

    const UpiId& payee() const noexcept { return payee_; }

    // UPI PINs are 4 or 6 digits. Throws ValidationError.
    static void validatePinFormat(const std::string& pin);

private:
    // Declared (and so constructed) before payee_: if the UPI ID is invalid,
    // pin_ is already a SecretString and its destructor wipes the PIN.
    SecretString pin_;
    UpiId payee_;
};

} // namespace paysphere::payments
