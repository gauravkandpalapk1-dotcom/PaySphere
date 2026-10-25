#pragma once

#include "models/User.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paysphere::db {

class DatabaseManager;
class Statement;

// A user together with their stored password hash. Only the authentication
// layer should ask for this; everything else uses models::User.
struct UserCredentials {
    models::User user;
    std::string passwordHash;
};

// Persistence for the users table (Repository pattern): all SQL about users
// lives here, so services deal only in models.
class UserRepository {
public:
    explicit UserRepository(DatabaseManager& database);

    // Inserts a new user (user.id() must be 0) and returns the stored row.
    // user.createdAt() is stored if set; otherwise the database's clock is used.
    // Throws ConstraintViolation if the email, UPI ID or phone is taken.
    models::User insert(const models::User& user, const std::string& passwordHash);

    std::optional<models::User> findById(std::int64_t id);
    // Exact match on a normalized (lowercase) UPI ID; the column ignores case anyway.
    std::optional<models::User> findByUpiId(const std::string& upiId);
    // Looks the identifier up as an email first, then as a UPI ID (both are
    // matched case-insensitively). They cannot collide: validation requires
    // a dot in an email's domain and forbids one in a UPI handle's provider.
    std::optional<UserCredentials> findCredentialsByLogin(const std::string& identifier);

    bool emailExists(const std::string& email);
    bool upiIdExists(const std::string& upiId);
    bool phoneExists(const std::string& phone);

    void updatePasswordHash(std::int64_t userId, const std::string& passwordHash);

private:
    static models::User toUser(const Statement& row);
    bool exists(const char* sql, const std::string& value);

    DatabaseManager& database_;
};

} // namespace paysphere::db
