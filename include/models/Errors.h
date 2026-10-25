#pragma once

#include <stdexcept>
#include <string>

namespace paysphere {

// Root of the PaySphere exception hierarchy. Every layer throws a subclass, so
// callers can catch PaySphereError for "any expected failure" and still tell
// categories apart through the virtual code().
class PaySphereError : public std::runtime_error {
public:
    explicit PaySphereError(const std::string& message);
    ~PaySphereError() override;

    // Stable, machine-readable category for logs and user-facing messages.
    virtual const char* code() const noexcept;
};

// Input or object state that breaks a business rule.
class ValidationError : public PaySphereError {
public:
    explicit ValidationError(const std::string& message);

    const char* code() const noexcept override;
};

// Registration conflicts with an existing account. field() names the clashing
// field: "email", "upi_id" or "phone".
class DuplicateAccountError : public PaySphereError {
public:
    DuplicateAccountError(std::string field, const std::string& message);

    const char* code() const noexcept override;
    const std::string& field() const noexcept { return field_; }

private:
    std::string field_;
};

// Wrong credentials. The message is deliberately the same whether the account
// exists or not, so it cannot be used to discover registered users.
class AuthenticationError : public PaySphereError {
public:
    explicit AuthenticationError(const std::string& message);

    const char* code() const noexcept override;
};

// Too many failed logins; further attempts are refused until lockedUntil()
// (a UTC timestamp such as "2026-10-14T05:45:00Z").
class AccountLockedError : public AuthenticationError {
public:
    AccountLockedError(const std::string& message, std::string lockedUntil);

    const char* code() const noexcept override;
    const std::string& lockedUntil() const noexcept { return lockedUntil_; }

private:
    std::string lockedUntil_;
};

// An authenticated user asked for something that belongs to someone else.
// Used for both "not yours" and "does not exist", so it cannot be used to
// probe which ids exist.
class AuthorizationError : public PaySphereError {
public:
    explicit AuthorizationError(const std::string& message);

    const char* code() const noexcept override;
};

// Payments are temporarily blocked for this user (e.g. after repeated wrong
// UPI PINs) until blockedUntil(), a UTC timestamp.
class PaymentBlockedError : public PaySphereError {
public:
    PaymentBlockedError(const std::string& message, std::string blockedUntil);

    const char* code() const noexcept override;
    const std::string& blockedUntil() const noexcept { return blockedUntil_; }

private:
    std::string blockedUntil_;
};

// An idempotency key was reused for a *different* payment request. Reusing a
// key for the same request is fine: it returns the original result.
class IdempotencyConflictError : public PaySphereError {
public:
    explicit IdempotencyConflictError(const std::string& message);

    const char* code() const noexcept override;
};

// A session token that is unknown, malformed, expired or logged out.
class SessionError : public AuthenticationError {
public:
    explicit SessionError(const std::string& message);

    const char* code() const noexcept override;
};

} // namespace paysphere
