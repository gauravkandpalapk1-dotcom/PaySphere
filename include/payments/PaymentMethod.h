#pragma once

#include "payments/Money.h"

#include <cstdint>
#include <string>

namespace paysphere::payments {

// Checks a payer's secret credential (a UPI PIN, a card OTP, ...).
//
// Declared here and implemented by the services layer (UpiPinService), so the
// payments layer depends only on this abstraction, never on services
// (dependency inversion).
class CredentialVerifier {
public:
    virtual ~CredentialVerifier();
    virtual bool verifyCredential(std::int64_t userId, const std::string& credential) = 0;
};

// Abstract base for a way of paying. A PaymentMethod describes one payment
// request: who is paid, how much, and how the payer proves consent. It never
// moves money itself; services::PaymentService does that, the same way for
// every method (polymorphism through the virtual functions below).
//
// Not copyable: copying through a base reference would slice, and a method
// may hold a secret that should exist in exactly one place.
class PaymentMethod {
public:
    virtual ~PaymentMethod();

    PaymentMethod(const PaymentMethod&) = delete;
    PaymentMethod& operator=(const PaymentMethod&) = delete;

    Money amount() const noexcept { return amount_; }

    // Short name such as "UPI".
    virtual const char* methodName() const noexcept = 0;
    // The payee in this method's own addressing scheme (a UPI ID for UPI).
    virtual const std::string& payeeAddress() const noexcept = 0;
    // Asks the verifier whether the payer's credential is correct.
    virtual bool authorize(std::int64_t payerUserId, CredentialVerifier& verifier) const = 0;

    // A canonical description of what is requested (method, payee, amount),
    // without any secret. Two requests with the same descriptor are the same
    // payment, which is how a retried request is recognized (idempotency).
    std::string requestDescriptor() const;

protected:
    // Throws ValidationError unless the amount is positive.
    explicit PaymentMethod(Money amount);

private:
    Money amount_;
};

} // namespace paysphere::payments
