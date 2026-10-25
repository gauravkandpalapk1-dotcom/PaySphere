#pragma once

#include "database/LoginAttemptRepository.h"
#include "database/UserRepository.h"
#include "database/WalletRepository.h"
#include "models/User.h"

#include <chrono>
#include <optional>
#include <string>

namespace paysphere::db {
class DatabaseManager;
}

namespace paysphere::services {

class Clock;
class PasswordHasher;
class SessionManager;

struct RegistrationRequest {
    std::string fullName;
    std::string email;
    std::string phone;
    std::string upiId;
    std::string password;
};

struct LoginResult {
    std::string sessionToken; // secret: give to the client, never log it
    models::User user;
};

// Temporary lockout after repeated failed logins (brute-force protection).
struct LockoutPolicy {
    int maxFailures = 5;                                        // within...
    std::chrono::seconds window{std::chrono::minutes(15)};      // ...this period
    std::chrono::seconds lockoutDuration{std::chrono::minutes(15)};
};

// Registration, login, logout and session checks.
//
// Collaborators arrive through the constructor (dependency injection), so
// tests can supply an in-memory database, a manual clock and a fast hasher.
class AuthService {
public:
    AuthService(db::DatabaseManager& database, SessionManager& sessions,
                const PasswordHasher& hasher, const Clock& clock, LockoutPolicy policy = {});

    // Validates the request, hashes the password and creates the user and
    // their zero-balance wallet in one database transaction: both rows exist
    // or neither does.
    // Throws ValidationError or DuplicateAccountError.
    models::User registerUser(const RegistrationRequest& request);

    // identifier is the email or UPI ID (case-insensitive).
    // Throws AuthenticationError (same message whether or not the account
    // exists) or AccountLockedError.
    LoginResult login(const std::string& identifier, const std::string& password);

    // Ends the session. Safe to call with an unknown or expired token.
    void logout(const std::string& sessionToken);

    // The user behind a live session; refreshes the session's idle timer.
    // Throws SessionError.
    models::User authenticate(const std::string& sessionToken);

    const LockoutPolicy& lockoutPolicy() const noexcept { return policy_; }

private:
    static std::string lockoutKey(const std::string& identifier, bool accountExists);
    // Throws AccountLockedError if the key has too many recent failures.
    void throwIfLocked(const std::string& key, std::chrono::system_clock::time_point now);

    db::DatabaseManager& database_;
    SessionManager& sessions_;
    const PasswordHasher& hasher_;
    const Clock& clock_;
    LockoutPolicy policy_;
    db::UserRepository users_;
    db::WalletRepository wallets_;
    db::LoginAttemptRepository loginAttempts_;
    std::string dummyHash_;
};

} // namespace paysphere::services
