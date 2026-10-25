#include "models/Errors.h"

#include <utility>

namespace paysphere {

PaySphereError::PaySphereError(const std::string& message)
    : std::runtime_error(message) {}

PaySphereError::~PaySphereError() = default;

const char* PaySphereError::code() const noexcept {
    return "PAYSPHERE_ERROR";
}

ValidationError::ValidationError(const std::string& message)
    : PaySphereError(message) {}

const char* ValidationError::code() const noexcept {
    return "VALIDATION_ERROR";
}

DuplicateAccountError::DuplicateAccountError(std::string field, const std::string& message)
    : PaySphereError(message), field_(std::move(field)) {}

const char* DuplicateAccountError::code() const noexcept {
    return "DUPLICATE_ACCOUNT";
}

AuthenticationError::AuthenticationError(const std::string& message)
    : PaySphereError(message) {}

const char* AuthenticationError::code() const noexcept {
    return "AUTHENTICATION_FAILED";
}

AccountLockedError::AccountLockedError(const std::string& message, std::string lockedUntil)
    : AuthenticationError(message), lockedUntil_(std::move(lockedUntil)) {}

const char* AccountLockedError::code() const noexcept {
    return "ACCOUNT_LOCKED";
}

AuthorizationError::AuthorizationError(const std::string& message)
    : PaySphereError(message) {}

const char* AuthorizationError::code() const noexcept {
    return "FORBIDDEN";
}

PaymentBlockedError::PaymentBlockedError(const std::string& message, std::string blockedUntil)
    : PaySphereError(message), blockedUntil_(std::move(blockedUntil)) {}

const char* PaymentBlockedError::code() const noexcept {
    return "PAYMENT_BLOCKED";
}

IdempotencyConflictError::IdempotencyConflictError(const std::string& message)
    : PaySphereError(message) {}

const char* IdempotencyConflictError::code() const noexcept {
    return "IDEMPOTENCY_CONFLICT";
}

SessionError::SessionError(const std::string& message)
    : AuthenticationError(message) {}

const char* SessionError::code() const noexcept {
    return "SESSION_INVALID";
}

} // namespace paysphere
