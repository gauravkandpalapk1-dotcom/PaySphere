#include "database/UserRepository.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "database/Statement.h"
#include "models/Errors.h"

#include <sqlite3.h>

namespace paysphere::db {

namespace {

// Column order expected by UserRepository::toUser.
constexpr const char* kUserColumns = "id, full_name, email, phone, upi_id, created_at";

} // namespace

UserRepository::UserRepository(DatabaseManager& database) : database_(database) {}

models::User UserRepository::toUser(const Statement& row) {
    try {
        return models::User(row.columnInt64(0),
                            row.columnText(1),
                            row.columnIsNull(2) ? std::string() : row.columnText(2), // pre-v3 account
                            row.columnText(3),
                            row.columnText(4),
                            row.columnText(5));
    } catch (const ValidationError& error) {
        throw CorruptDataError(std::string("Stored user row is invalid: ") + error.what());
    }
}

models::User UserRepository::insert(const models::User& user, const std::string& passwordHash) {
    if (user.isPersisted()) {
        throw DatabaseError("Cannot insert a user that already has an id", SQLITE_MISUSE);
    }
    const std::int64_t id = database_.insert(
        "INSERT INTO users (full_name, email, phone, upi_id, password_hash, created_at) "
        "VALUES (?, ?, ?, ?, ?, coalesce(?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now')))",
        {user.fullName(), user.email(), user.phone(), user.upiId(), passwordHash,
         user.createdAt().empty() ? SqlValue(nullptr) : SqlValue(user.createdAt())});

    std::optional<models::User> stored = findById(id);
    if (!stored) {
        throw DatabaseError("Inserted user could not be read back", SQLITE_INTERNAL);
    }
    return *stored;
}

std::optional<models::User> UserRepository::findById(std::int64_t id) {
    Statement query =
        database_.prepare(std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?", {id});
    if (!query.step()) {
        return std::nullopt;
    }
    return toUser(query);
}

std::optional<models::User> UserRepository::findByUpiId(const std::string& upiId) {
    Statement query = database_.prepare(
        std::string("SELECT ") + kUserColumns + " FROM users WHERE upi_id = ?", {upiId});
    if (!query.step()) {
        return std::nullopt;
    }
    return toUser(query);
}

std::optional<UserCredentials> UserRepository::findCredentialsByLogin(const std::string& identifier) {
    for (const char* column : {"email", "upi_id"}) {
        Statement query = database_.prepare(std::string("SELECT ") + kUserColumns +
                                                ", password_hash FROM users WHERE " + column + " = ?",
                                            {identifier});
        if (query.step()) {
            return UserCredentials{toUser(query), query.columnText(6)};
        }
    }
    return std::nullopt;
}

bool UserRepository::exists(const char* sql, const std::string& value) {
    return database_.queryInt64(sql, {value}).has_value();
}

bool UserRepository::emailExists(const std::string& email) {
    return exists("SELECT 1 FROM users WHERE email = ?", email);
}

bool UserRepository::upiIdExists(const std::string& upiId) {
    return exists("SELECT 1 FROM users WHERE upi_id = ?", upiId);
}

bool UserRepository::phoneExists(const std::string& phone) {
    return exists("SELECT 1 FROM users WHERE phone = ?", phone);
}

void UserRepository::updatePasswordHash(std::int64_t userId, const std::string& passwordHash) {
    if (database_.execute("UPDATE users SET password_hash = ? WHERE id = ?", {passwordHash, userId}) != 1) {
        throw DatabaseError("No user with id " + std::to_string(userId), SQLITE_NOTFOUND);
    }
}

} // namespace paysphere::db
