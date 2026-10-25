#include "services/AuthService.h"

#include "Sodium.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "services/Clock.h"
#include "services/InputValidator.h"
#include "services/PasswordHasher.h"
#include "services/SessionManager.h"

#include <sstream>
#include <vector>

namespace paysphere::services {

namespace {

// One message for "no such account" and "wrong password", so a login form
// cannot be used to discover which emails or UPI IDs are registered.
constexpr const char* kInvalidCredentials = "Incorrect email/UPI ID or password";

std::string localPart(const std::string& address) {
    return address.substr(0, address.find('@'));
}

// Words a password must not contain: name parts, email name, UPI handle, phone.
std::vector<std::string> personalDetails(const std::string& fullName, const std::string& email,
                                         const std::string& upiId, const std::string& phone) {
    std::vector<std::string> details{localPart(email), localPart(upiId), phone};
    std::istringstream words(fullName);
    for (std::string word; words >> word;) {
        details.push_back(word);
    }
    return details;
}

} // namespace

AuthService::AuthService(db::DatabaseManager& database, SessionManager& sessions,
                         const PasswordHasher& hasher, const Clock& clock, LockoutPolicy policy)
    : database_(database),
      sessions_(sessions),
      hasher_(hasher),
      clock_(clock),
      policy_(policy),
      users_(database),
      wallets_(database),
      loginAttempts_(database) {
    if (policy_.maxFailures < 1 || policy_.window.count() <= 0 || policy_.lockoutDuration.count() <= 0) {
        throw ValidationError("Lockout policy values must be positive");
    }
    // Computed now rather than on first use, so even the first login for an
    // unknown account costs exactly one Argon2id verification. Same
    // algorithm and cost as real hashes; its value never matters because
    // login also requires the account to exist.
    dummyHash_ = hasher_.hash("paysphere-timing-equalizer");
}

models::User AuthService::registerUser(const RegistrationRequest& request) {
    const std::string fullName = InputValidator::fullName(request.fullName);
    const std::string email = InputValidator::email(request.email);
    const std::string phone = InputValidator::phone(request.phone);
    const std::string upiId = InputValidator::upiId(request.upiId);
    InputValidator::password(request.password, personalDetails(fullName, email, upiId, phone));

    // Hash before opening the transaction: Argon2id is deliberately slow and
    // the write lock should not be held meanwhile.
    const std::string passwordHash = hasher_.hash(request.password);

    try {
        return database_.inTransaction([&] {
            // The transaction holds the write lock, so no other registration
            // can claim these values between the checks and the insert.
            if (users_.emailExists(email)) {
                throw DuplicateAccountError("email", "An account with this email already exists");
            }
            if (users_.upiIdExists(upiId)) {
                throw DuplicateAccountError("upi_id", "This UPI ID is already taken");
            }
            if (users_.phoneExists(phone)) {
                throw DuplicateAccountError("phone", "An account with this phone number already exists");
            }
            const std::string createdAt = formatTimestamp(clock_.now());
            models::User user =
                users_.insert(models::User(0, fullName, email, phone, upiId, createdAt), passwordHash);
            wallets_.createForUser(user.id(), createdAt);
            return user;
        });
    } catch (const db::ConstraintViolation& error) {
        // Backstop: the UNIQUE indexes still win if the checks above missed a case.
        if (error.isUniqueViolation()) {
            throw DuplicateAccountError("account", "An account with these details already exists");
        }
        throw;
    }
}

LoginResult AuthService::login(const std::string& identifierInput, const std::string& password) {
    const std::string identifier = InputValidator::loginIdentifier(identifierInput);
    const TimePoint now = clock_.now();
    const std::optional<db::UserCredentials> account = users_.findCredentialsByLogin(identifier);
    const std::string key = lockoutKey(identifier, account.has_value());

    // While locked, refuse without checking the password and without
    // recording the attempt, so retries cannot extend the lockout.
    throwIfLocked(key, now);

    // Exactly one Argon2id verification happens on every path, against a
    // dummy hash when the account does not exist, so response time does not
    // reveal whether an identifier is registered. It runs outside the write
    // lock because it is deliberately slow.
    const bool lengthOk = password.size() <= InputValidator::kMaxPasswordLength;
    const std::string candidate = lengthOk ? password : std::string();
    const bool hashMatches = hasher_.verify(account ? account->passwordHash : dummyHash_, candidate);
    const bool succeeded = account.has_value() && lengthOk && hashMatches;

    std::optional<std::int64_t> userId;
    if (account) {
        userId = account->user.id();
    }
    // Check the lock again and record the attempt as one atomic step. A burst
    // of parallel guesses all pass the first check, but they are serialized
    // here: once the limit is reached, the rest are refused without revealing
    // whether their password was right (and are not recorded).
    database_.inTransaction([&] {
        throwIfLocked(key, now);
        loginAttempts_.record(key, userId, succeeded, formatTimestamp(now));
    });

    if (!succeeded) {
        throwIfLocked(key, now); // this failure may have reached the limit
        throw AuthenticationError(kInvalidCredentials);
    }

    // Upgrade hashes made with older, weaker parameters while the plaintext
    // is briefly available.
    if (hasher_.needsRehash(account->passwordHash)) {
        users_.updatePasswordHash(account->user.id(), hasher_.hash(password));
    }
    return LoginResult{sessions_.create(account->user.id()), account->user};
}

void AuthService::logout(const std::string& sessionToken) {
    sessions_.revoke(sessionToken);
}

models::User AuthService::authenticate(const std::string& sessionToken) {
    const models::Session session = sessions_.validate(sessionToken);
    std::optional<models::User> user = users_.findById(session.userId());
    if (!user) {
        sessions_.revoke(sessionToken);
        throw SessionError("Your session is invalid or has expired; please log in again");
    }
    return *user;
}

std::string AuthService::lockoutKey(const std::string& identifier, bool accountExists) {
    // Lockouts count per identifier as typed. Counting per account instead
    // would let an attacker link an email to a UPI ID: 4 failures as the UPI
    // ID, then 1 per candidate email, and only the right email would lock.
    //
    // For unknown identifiers only a digest is stored: people sometimes type
    // their password into the email field, and that must not end up in the
    // database in clear text.
    return accountExists ? identifier : "unknown:" + detail::blake2bHex(identifier);
}

void AuthService::throwIfLocked(const std::string& key, TimePoint now) {
    const db::FailureSummary failures = loginAttempts_.recentFailures(key, formatTimestamp(now - policy_.window));
    if (failures.failures < policy_.maxFailures || !failures.lastFailureAt) {
        return;
    }
    const TimePoint until = parseTimestamp(*failures.lastFailureAt) + policy_.lockoutDuration;
    if (now < until) {
        const std::string untilText = formatTimestamp(until);
        throw AccountLockedError("Too many failed login attempts; try again after " + untilText, untilText);
    }
}

} // namespace paysphere::services
