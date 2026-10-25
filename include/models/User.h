#pragma once

#include <cstdint>
#include <string>

namespace paysphere::models {

// A registered account holder, as shown to the rest of the application.
//
// The password hash is deliberately not part of this class: code outside the
// authentication layer never needs it, so a User passed around the program
// (or returned after login) cannot leak it. Credentials travel only in
// db::UserCredentials. An id of 0 means "not yet saved to the database".
//
// email() is empty only for accounts created before schema v3 introduced
// email. Every account registered since has one: the database enforces this
// with triggers, and registration validates it.
class User {
public:
    User(std::int64_t id,
         std::string fullName,
         std::string email,
         std::string phone,
         std::string upiId,
         std::string createdAt = {});

    std::int64_t id() const noexcept { return id_; }
    bool isPersisted() const noexcept { return id_ > 0; }
    const std::string& fullName() const noexcept { return fullName_; }
    const std::string& email() const noexcept { return email_; }
    const std::string& phone() const noexcept { return phone_; }
    const std::string& upiId() const noexcept { return upiId_; }
    const std::string& createdAt() const noexcept { return createdAt_; }

private:
    std::int64_t id_;
    std::string fullName_;
    std::string email_;
    std::string phone_;
    std::string upiId_;
    std::string createdAt_;
};

} // namespace paysphere::models
