// Building blocks of authentication: input validation, timestamps, password hashing.

#include "TestFramework.h"

#include "models/Errors.h"
#include "services/Clock.h"
#include "services/InputValidator.h"
#include "services/PasswordHasher.h"

#include <chrono>
#include <string>

using paysphere::ValidationError;
using paysphere::services::formatTimestamp;
using paysphere::services::InputValidator;
using paysphere::services::parseTimestamp;
using paysphere::services::SodiumPasswordHasher;

// ------------------------------------------------------------------ inputs

TEST(validator_normalizes_valid_input) {
    CHECK_EQ(InputValidator::fullName("  Asha Verma "), "Asha Verma");
    CHECK_EQ(InputValidator::email(" Asha.Verma+pay@Example.CO.in "), "asha.verma+pay@example.co.in");
    CHECK_EQ(InputValidator::phone("+91 98765-43210"), "9876543210");
    CHECK_EQ(InputValidator::phone("6000000000"), "6000000000");
    CHECK_EQ(InputValidator::upiId(" Asha.V_1@PaySphere "), "asha.v_1@paysphere");
    CHECK_EQ(InputValidator::loginIdentifier("  ASHA@Example.com "), "asha@example.com");
}

TEST(validator_rejects_bad_names) {
    for (const char* name : {"", " ", "A", "12345", "Asha\tVerma"}) {
        CHECK_THROWS_AS(InputValidator::fullName(name), ValidationError);
    }
    CHECK_THROWS_AS(InputValidator::fullName(std::string(101, 'a')), ValidationError);
}

TEST(validator_rejects_bad_emails) {
    for (const char* email : {"", "asha", "asha@", "@example.com", "asha@example", "asha@@example.com",
                              "as ha@example.com", ".asha@example.com", "asha.@example.com",
                              "as..ha@example.com", "asha@-example.com", "asha@example.c0m",
                              "asha@example..com", "asha@example.com."}) {
        CHECK_THROWS_AS(InputValidator::email(email), ValidationError);
    }
    CHECK_THROWS_AS(InputValidator::email(std::string(95, 'a') + "@x.com"), ValidationError); // > 100
}

TEST(validator_rejects_bad_phones) {
    for (const char* phone : {"", "12345", "5876543210", "98765432101", "98765abcde", "+1 9876543210"}) {
        CHECK_THROWS_AS(InputValidator::phone(phone), ValidationError);
    }
}

TEST(validator_rejects_bad_upi_ids) {
    for (const char* upi : {"", "asha", "as@paysphere", "asha@", "@paysphere", "asha@pay1",
                            "asha@pay.sphere", "_asha@paysphere", "asha!@paysphere", "asha@p"}) {
        CHECK_THROWS_AS(InputValidator::upiId(upi), ValidationError);
    }
}

TEST(validator_applies_nist_style_password_rules) {
    InputValidator::password("correct horse battery", {});
    InputValidator::password("Tr1cky!pass", {"asha", "9876543210"});

    CHECK_THROWS_AS(InputValidator::password("short1!", {}), ValidationError);
    CHECK_THROWS_AS(InputValidator::password(std::string(129, 'x'), {}), ValidationError);
    CHECK_THROWS_AS(InputValidator::password("        ", {}), ValidationError);
    CHECK_THROWS_AS(InputValidator::password("Password123", {}), ValidationError); // common, any case
    CHECK_THROWS_AS(InputValidator::password("asha-secret-1", {"asha"}), ValidationError);
    CHECK_THROWS_AS(InputValidator::password("x9876543210x", {"9876543210"}), ValidationError);
}

// -------------------------------------------------------------- timestamps

TEST(timestamps_round_trip_and_reject_invalid_dates) {
    const std::string text = "2026-10-14T05:30:09Z";
    CHECK_EQ(formatTimestamp(parseTimestamp(text)), text);
    CHECK_EQ(formatTimestamp(parseTimestamp("1970-01-01T00:00:00Z") + std::chrono::hours(24)),
             "1970-01-02T00:00:00Z");
    CHECK_EQ(formatTimestamp(parseTimestamp("2028-02-28T23:59:59Z") + std::chrono::seconds(1)),
             "2028-02-29T00:00:00Z"); // leap year

    for (const char* bad : {"", "2026-10-14 05:30:09Z", "2026-10-14T05:30:09", "2026-13-01T00:00:00Z",
                            "2026-02-30T00:00:00Z", "2027-02-29T00:00:00Z", "2026-10-14T24:00:00Z",
                            "2026-10-14T05:60:00Z", "2026-1O-14T05:30:09Z"}) {
        CHECK_THROWS_AS(parseTimestamp(bad), ValidationError);
    }
}

// --------------------------------------------------------- password hashing

TEST(hasher_produces_salted_argon2id_hashes) {
    const SodiumPasswordHasher hasher(SodiumPasswordHasher::Cost::minimumForTests());
    const std::string first = hasher.hash("correct horse battery");
    const std::string second = hasher.hash("correct horse battery");

    CHECK_EQ(first.rfind("$argon2id$", 0), 0u);
    CHECK(first != second); // a random salt per hash
    CHECK(first.find("correct horse") == std::string::npos);
    CHECK(hasher.verify(first, "correct horse battery"));
    CHECK(hasher.verify(second, "correct horse battery"));
    CHECK(!hasher.verify(first, "correct horse batterY"));
    CHECK(!hasher.verify(first, ""));
}

TEST(hasher_rejects_malformed_hashes_safely) {
    const SodiumPasswordHasher hasher(SodiumPasswordHasher::Cost::minimumForTests());
    CHECK(!hasher.verify("", "password"));
    CHECK(!hasher.verify("plaintext-password", "plaintext-password"));
    CHECK(!hasher.verify(std::string(500, '$'), "password"));
    CHECK(hasher.needsRehash("not-a-hash"));
}

TEST(hasher_default_cost_is_libsodium_interactive) {
    const SodiumPasswordHasher production;
    const std::string hash = production.hash("correct horse battery");
    // 64 MiB of memory (m is in KiB), 2 passes, 1 lane.
    CHECK(hash.find("$m=65536,t=2,p=1$") != std::string::npos);
    CHECK(production.verify(hash, "correct horse battery"));
    CHECK(!production.needsRehash(hash));

    const SodiumPasswordHasher weak(SodiumPasswordHasher::Cost::minimumForTests());
    CHECK(production.needsRehash(weak.hash("correct horse battery")));
}
