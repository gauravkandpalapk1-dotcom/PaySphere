#include "models/User.h"

#include "models/Errors.h"

#include <utility>

namespace paysphere::models {

// The constructor enforces structural invariants only. Format rules for user
// input (email, phone, UPI ID syntax) are applied by services::InputValidator
// before a User is created.
User::User(std::int64_t id,
           std::string fullName,
           std::string email,
           std::string phone,
           std::string upiId,
           std::string createdAt)
    : id_(id),
      fullName_(std::move(fullName)),
      email_(std::move(email)),
      phone_(std::move(phone)),
      upiId_(std::move(upiId)),
      createdAt_(std::move(createdAt)) {
    if (id_ < 0) {
        throw ValidationError("User id must not be negative");
    }
    if (fullName_.empty()) {
        throw ValidationError("User full name is required");
    }
    if (phone_.empty()) {
        throw ValidationError("User phone number is required");
    }
    if (upiId_.empty()) {
        throw ValidationError("User UPI ID is required");
    }
}

} // namespace paysphere::models
