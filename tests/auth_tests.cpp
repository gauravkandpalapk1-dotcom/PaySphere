// AuthService and SessionManager: registration, login, lockout, sessions.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/DatabaseManager.h"
#include "database/WalletRepository.h"
#include "models/Errors.h"
#include "services/AuthService.h"
#include "services/PasswordHasher.h"
#include "services/SessionManager.h"

#include <chrono>
#include <cstdint>
#include <set>
#include <string>

using paysphere::AccountLockedError;
using paysphere::AuthenticationError;
using paysphere::DuplicateAccountError;
using paysphere::SessionError;
using paysphere::ValidationError;
using paysphere::db::DatabaseManager;
using paysphere::db::WalletRepository;
using paysphere::services::AuthService;
using paysphere::services::LoginResult;
using paysphere::services::RegistrationRequest;
using paysphere::services::SessionManager;
using paysphere::services::SodiumPasswordHasher;
using testsupport::ManualClock;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPassword = "correct horse battery";

// A complete, isolated system: in-memory database, manual clock and the
// cheapest Argon2id parameters (hashing at production cost would make the
// suite slow; production cost is tested in test_validation.cpp).
struct AuthFixture {
    ManualClock clock;
    DatabaseManager db{":memory:"};
    SodiumPasswordHasher hasher{SodiumPasswordHasher::Cost::minimumForTests()};
    SessionManager sessions{clock};
    AuthService auth{db, sessions, hasher, clock};

    AuthFixture() { db.initialize(); }

    static RegistrationRequest asha() {
        return {"Asha Verma", "asha@example.com", "9876543210", "asha@paysphere", kPassword};
    }

    std::int64_t count(const std::string& sql) { return db.queryInt64(sql).value(); }

    // Fails `times` logins with a wrong password, ignoring the exceptions.
    void failLogins(const std::string& identifier, int times) {
        for (int i = 0; i < times; ++i) {
            try {
                auth.login(identifier, "wrong password 123");
            } catch (const AuthenticationError&) {
            }
        }
    }
};

bool isHexToken(const std::string& token) {
    return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string::npos;
}

} // namespace

// ------------------------------------------------------------ registration

TEST(register_creates_user_and_empty_wallet) {
    AuthFixture f;
    RegistrationRequest request = AuthFixture::asha();
    request.email = "  Asha@Example.COM ";
    request.upiId = "Asha@PaySphere";
    request.phone = "+91 98765 43210";

    const auto user = f.auth.registerUser(request);
    CHECK(user.isPersisted());
    CHECK_EQ(user.fullName(), "Asha Verma");
    CHECK_EQ(user.email(), "asha@example.com"); // normalized
    CHECK_EQ(user.upiId(), "asha@paysphere");
    CHECK_EQ(user.phone(), "9876543210");
    CHECK(!user.createdAt().empty());

    WalletRepository wallets(f.db);
    const auto wallet = wallets.findByUserId(user.id());
    CHECK(wallet.has_value());
    CHECK(wallet->balance().isZero());
}

TEST(register_stores_only_a_salted_argon2id_hash) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.auth.registerUser({"Ravi Kumar", "ravi@example.com", "9123456789", "ravi@paysphere", kPassword});

    const std::string ashaHash =
        f.db.queryText("SELECT password_hash FROM users WHERE upi_id = 'asha@paysphere'").value();
    const std::string raviHash =
        f.db.queryText("SELECT password_hash FROM users WHERE upi_id = 'ravi@paysphere'").value();
    CHECK_EQ(ashaHash.rfind("$argon2id$", 0), 0u);
    CHECK(ashaHash.find(kPassword) == std::string::npos);
    CHECK(ashaHash != raviHash); // same password, different salts
    CHECK_EQ(f.count("SELECT count(*) FROM users WHERE password_hash LIKE '%horse%'"), 0);
}

TEST(register_rejects_duplicate_email_upi_id_and_phone) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());

    RegistrationRequest sameEmail{"Other", "ASHA@example.com", "9000000001", "other1@paysphere", kPassword};
    RegistrationRequest sameUpi{"Other", "other2@example.com", "9000000002", "ASHA@PAYSPHERE", kPassword};
    RegistrationRequest samePhone{"Other", "other3@example.com", "9876543210", "other3@paysphere", kPassword};

    try {
        f.auth.registerUser(sameEmail);
        CHECK(false);
    } catch (const DuplicateAccountError& error) {
        CHECK_EQ(error.field(), "email");
    }
    try {
        f.auth.registerUser(sameUpi);
        CHECK(false);
    } catch (const DuplicateAccountError& error) {
        CHECK_EQ(error.field(), "upi_id");
    }
    try {
        f.auth.registerUser(samePhone);
        CHECK(false);
    } catch (const DuplicateAccountError& error) {
        CHECK_EQ(error.field(), "phone");
    }
    // Nothing was half-created by the failed attempts.
    CHECK_EQ(f.count("SELECT count(*) FROM users"), 1);
    CHECK_EQ(f.count("SELECT count(*) FROM wallets"), 1);
}

TEST(register_rejects_invalid_input_without_writing) {
    AuthFixture f;
    auto with = [](auto change) {
        RegistrationRequest request = AuthFixture::asha();
        change(request);
        return request;
    };
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.fullName = " "; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.email = "not-an-email"; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.phone = "12345"; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.upiId = "asha"; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.password = "short"; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.password = "password123"; })), ValidationError);
    CHECK_THROWS_AS(f.auth.registerUser(with([](auto& r) { r.password = "verma-is-me-2026"; })),
                    ValidationError); // contains the user's name
    CHECK_EQ(f.count("SELECT count(*) FROM users"), 0);
}

TEST(register_is_atomic_when_wallet_creation_fails) {
    AuthFixture f;
    // Simulate a failure after the user row is written but before the wallet is.
    f.db.connection().execute(
        "CREATE TRIGGER fail_wallet BEFORE INSERT ON wallets BEGIN SELECT RAISE(ABORT, 'disk full'); END");
    CHECK_THROWS_AS(f.auth.registerUser(AuthFixture::asha()), paysphere::PaySphereError);
    CHECK_EQ(f.count("SELECT count(*) FROM users"), 0); // the user insert was rolled back

    f.db.connection().execute("DROP TRIGGER fail_wallet");
    f.auth.registerUser(AuthFixture::asha()); // and the same details work afterwards
    CHECK_EQ(f.count("SELECT count(*) FROM wallets"), 1);
}

// ------------------------------------------------------------------- login

TEST(login_succeeds_with_email_or_upi_id_in_any_case) {
    AuthFixture f;
    const auto registered = f.auth.registerUser(AuthFixture::asha());

    for (const char* identifier : {"asha@example.com", " ASHA@Example.com", "asha@paysphere", "Asha@PaySphere"}) {
        const LoginResult result = f.auth.login(identifier, kPassword);
        CHECK_EQ(result.user.id(), registered.id());
        CHECK(isHexToken(result.sessionToken));
    }
    CHECK_EQ(f.count("SELECT count(*) FROM login_attempts WHERE succeeded = 1 AND user_id IS NOT NULL"), 4);
}

TEST(login_rejects_incorrect_password_and_records_it) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());

    CHECK_THROWS_AS(f.auth.login("asha@example.com", "correct horse batterY"), AuthenticationError);
    CHECK_THROWS_AS(f.auth.login("asha@example.com", ""), AuthenticationError);
    CHECK_THROWS_AS(f.auth.login("asha@example.com", std::string(10000, 'x')), AuthenticationError);
    CHECK_EQ(f.count("SELECT count(*) FROM login_attempts WHERE succeeded = 0 AND user_id = 1"), 3);
    CHECK_EQ(f.sessions.activeCount(), 0u);
}

TEST(login_failure_message_does_not_reveal_if_account_exists) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    std::string wrongPassword;
    std::string unknownAccount;
    try {
        f.auth.login("asha@example.com", "wrong password 123");
    } catch (const AuthenticationError& error) {
        wrongPassword = error.what();
    }
    try {
        f.auth.login("nobody@example.com", "wrong password 123");
    } catch (const AuthenticationError& error) {
        unknownAccount = error.what();
    }
    CHECK(!wrongPassword.empty());
    CHECK_EQ(unknownAccount, wrongPassword);
    CHECK_EQ(f.count("SELECT count(*) FROM login_attempts WHERE user_id IS NULL"), 1);
}

TEST(login_upgrades_hashes_made_with_weaker_parameters) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    const std::string before = f.db.queryText("SELECT password_hash FROM users").value();

    // The same database, now served by a hasher with a higher cost.
    const SodiumPasswordHasher stronger({2, SodiumPasswordHasher::Cost::minimumForTests().memLimitBytes});
    AuthService upgraded(f.db, f.sessions, stronger, f.clock);
    upgraded.login("asha@example.com", kPassword);

    const std::string after = f.db.queryText("SELECT password_hash FROM users").value();
    CHECK(after != before);
    CHECK(after.find(",t=2,") != std::string::npos);
    CHECK(!stronger.needsRehash(after));
    upgraded.login("asha@paysphere", kPassword); // the new hash still verifies
}

// ----------------------------------------------------------------- lockout

TEST(lockout_after_five_failures_blocks_even_the_correct_password) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.failLogins("asha@example.com", 4);

    try {
        f.auth.login("asha@example.com", "wrong password 123");
        CHECK(false);
    } catch (const AccountLockedError& error) {
        CHECK_EQ(error.lockedUntil(), "2026-10-14T10:15:00Z"); // 15 minutes after the 5th failure
        CHECK_EQ(std::string(error.code()), "ACCOUNT_LOCKED");
    }
    CHECK_THROWS_AS(f.auth.login("asha@example.com", kPassword), AccountLockedError);
    // Attempts made while locked are refused outright and not recorded,
    // so retrying cannot extend the lockout.
    CHECK_EQ(f.count("SELECT count(*) FROM login_attempts"), 5);
}

TEST(lockout_expires_after_the_lockout_duration) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.failLogins("asha@example.com", 5);

    f.clock.advance(14min + 59s);
    CHECK_THROWS_AS(f.auth.login("asha@example.com", kPassword), AccountLockedError);
    f.clock.advance(1s);
    CHECK(isHexToken(f.auth.login("asha@example.com", kPassword).sessionToken));
}

TEST(lockout_is_per_identifier_so_it_cannot_link_email_and_upi_id) {
    // Regression test for an audit finding: lockouts used to be counted per
    // account, so 4 failures as the UPI ID plus 1 per candidate email locked
    // only the victim's real email, revealing which email owns the UPI ID.
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.failLogins("asha@paysphere", 4);                                     // 4 as the UPI ID
    CHECK_THROWS_AS(f.auth.login("asha@example.com", "wrong password 123"), // 1 as the real email
                    AuthenticationError);
    try {
        f.auth.login("asha@example.com", "wrong password 123");
    } catch (const AccountLockedError&) {
        CHECK(false); // the email must not be locked by failures on the UPI ID
    } catch (const AuthenticationError&) {
    }
    // Each identifier still locks on its own after 5 failures.
    f.failLogins("asha@paysphere", 1);
    CHECK_THROWS_AS(f.auth.login("asha@paysphere", kPassword), AccountLockedError);
    CHECK(isHexToken(f.auth.login("asha@example.com", kPassword).sessionToken));
}

TEST(lockout_ignores_failures_outside_the_window) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.failLogins("asha@example.com", 4);
    f.clock.advance(16min); // the first four age out of the 15-minute window
    f.failLogins("asha@example.com", 4);
    CHECK(isHexToken(f.auth.login("asha@example.com", kPassword).sessionToken));
}

TEST(successful_login_resets_the_failure_count) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    f.failLogins("asha@example.com", 4);
    f.auth.login("asha@example.com", kPassword);
    f.failLogins("asha@example.com", 4); // 8 failures in the window, but only 4 since the success
    CHECK(isHexToken(f.auth.login("asha@example.com", kPassword).sessionToken));
}

TEST(lockout_also_applies_to_unknown_identifiers) {
    AuthFixture f;
    f.failLogins("ghost@example.com", 4);
    CHECK_THROWS_AS(f.auth.login("ghost@example.com", "anything 1234"), AccountLockedError);
    // A real account is unaffected by someone guessing a different identifier.
    f.auth.registerUser(AuthFixture::asha());
    CHECK(isHexToken(f.auth.login("asha@example.com", kPassword).sessionToken));
}

// ---------------------------------------------------------------- sessions

TEST(session_identifies_the_logged_in_user) {
    AuthFixture f;
    const auto registered = f.auth.registerUser(AuthFixture::asha());
    const LoginResult login = f.auth.login("asha@example.com", kPassword);

    CHECK_EQ(f.auth.authenticate(login.sessionToken).id(), registered.id());
    CHECK_EQ(f.sessions.activeCount(), 1u);
}

TEST(logout_ends_the_session) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    const std::string token = f.auth.login("asha@example.com", kPassword).sessionToken;

    f.auth.logout(token);
    CHECK_THROWS_AS(f.auth.authenticate(token), SessionError);
    f.auth.logout(token);           // logging out twice is harmless
    f.auth.logout("not-a-token");   // so is an unknown token
    CHECK_EQ(f.sessions.activeCount(), 0u);
}

TEST(sessions_reject_forged_or_malformed_tokens) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    std::string token = f.auth.login("asha@example.com", kPassword).sessionToken;

    for (const std::string& bad : {std::string(), std::string("abc"), std::string(64, '0'),
                                   std::string(64, 'g'), token + "0", token.substr(1)}) {
        CHECK_THROWS_AS(f.auth.authenticate(bad), SessionError);
    }
    token[0] = token[0] == 'a' ? 'b' : 'a'; // one character changed
    CHECK_THROWS_AS(f.auth.authenticate(token), SessionError);
}

TEST(each_login_gets_a_distinct_random_session) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    std::set<std::string> tokens;
    for (int i = 0; i < 20; ++i) {
        tokens.insert(f.auth.login("asha@example.com", kPassword).sessionToken);
    }
    CHECK_EQ(tokens.size(), 20u);

    // Logging out one device leaves the others signed in.
    f.auth.logout(*tokens.begin());
    CHECK_EQ(f.sessions.activeCount(), 19u);
    CHECK_EQ(f.sessions.revokeAllForUser(1), 19u);
    CHECK_EQ(f.sessions.activeCount(), 0u);
}

TEST(session_expires_after_idle_timeout_but_activity_extends_it) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    const std::string token = f.auth.login("asha@example.com", kPassword).sessionToken;

    f.clock.advance(29min);
    f.auth.authenticate(token); // activity resets the 30-minute idle timer
    f.clock.advance(29min);
    f.auth.authenticate(token);
    f.clock.advance(30min);
    CHECK_THROWS_AS(f.auth.authenticate(token), SessionError);
    CHECK_EQ(f.sessions.activeCount(), 0u);
}

TEST(session_expires_after_absolute_timeout_despite_activity) {
    AuthFixture f;
    f.auth.registerUser(AuthFixture::asha());
    const std::string token = f.auth.login("asha@example.com", kPassword).sessionToken;

    for (int i = 0; i < 24; ++i) { // active every 29 minutes for 11 h 36 min
        f.clock.advance(29min);
        f.auth.authenticate(token);
    }
    f.clock.advance(25min); // 12 h 01 min after login
    CHECK_THROWS_AS(f.auth.authenticate(token), SessionError);
}

TEST(session_ends_if_the_account_no_longer_exists) {
    AuthFixture f;
    const auto user = f.auth.registerUser(AuthFixture::asha());
    const std::string token = f.auth.login("asha@example.com", kPassword).sessionToken;

    f.db.execute("DELETE FROM wallets WHERE user_id = ?", {user.id()});
    f.db.execute("DELETE FROM users WHERE id = ?", {user.id()});
    CHECK_THROWS_AS(f.auth.authenticate(token), SessionError);
    CHECK_EQ(f.sessions.activeCount(), 0u);
}
