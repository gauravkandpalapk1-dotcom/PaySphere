// TransactionService: history, filters, search, receipts, access control,
// immutability, auditing and persistence across restarts.

#include "TestFramework.h"
#include "TestSupport.h"

#include "database/DatabaseError.h"
#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "models/Transaction.h"
#include "payments/Money.h"
#include "payments/UPIPayment.h"
#include "services/AuthService.h"
#include "services/Clock.h"
#include "services/PasswordHasher.h"
#include "services/PaymentService.h"
#include "services/ReferenceGenerator.h"
#include "services/RiskEngine.h"
#include "services/SessionManager.h"
#include "services/TransactionService.h"
#include "services/UpiPinService.h"
#include "services/WalletService.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using paysphere::AuthorizationError;
using paysphere::SessionError;
using paysphere::ValidationError;
using paysphere::db::ConstraintViolation;
using paysphere::db::DatabaseManager;
using paysphere::models::Transaction;
using paysphere::models::TransactionDirection;
using paysphere::models::TransactionStatus;
using paysphere::models::TransactionType;
using paysphere::payments::Money;
using paysphere::payments::UPIPayment;
using paysphere::services::AuthService;
using paysphere::services::formatIndianTime;
using paysphere::services::HistoryEntry;
using paysphere::services::HistoryPage;
using paysphere::services::HistoryQuery;
using paysphere::services::parseTimestamp;
using paysphere::services::PaymentService;
using paysphere::services::RandomReferenceGenerator;
using paysphere::services::RiskEngine;
using paysphere::services::SessionManager;
using paysphere::services::SodiumPasswordHasher;
using paysphere::services::TransactionService;
using paysphere::services::UpiPinService;
using paysphere::services::WalletService;
using testsupport::ManualClock;
using testsupport::TempDirectory;
using namespace std::chrono_literals;

namespace {

constexpr const char* kPassword = "correct horse battery";
constexpr const char* kPin = "1357";

Money rupees(const char* amount) {
    return Money::parse(amount);
}

// The whole application on one database, with a manual clock.
struct AppFixture {
    ManualClock clock;
    DatabaseManager db;
    SodiumPasswordHasher hasher{SodiumPasswordHasher::Cost::minimumForTests()};
    SessionManager sessions{clock};
    AuthService auth{db, sessions, hasher, clock};
    RandomReferenceGenerator references;
    WalletService wallets{db, auth, references, clock};
    UpiPinService pins{db, auth, hasher, clock};
    RiskEngine risk{db, clock};
    PaymentService payments{db, auth, pins, risk, references, clock};
    TransactionService transactions{db, auth};

    explicit AppFixture(const std::string& path = ":memory:") : db(path) { db.initialize(); }

    std::string login(const std::string& name) {
        return auth.login(name + "@example.com", kPassword).sessionToken;
    }
    std::string signUp(const std::string& name, const std::string& phone) {
        std::string fullName = name + " Test"; // "asha" -> "Asha Test"
        fullName[0] = static_cast<char>(fullName[0] - 'a' + 'A');
        auth.registerUser({fullName, name + "@example.com", phone, name + "@paysphere", kPassword});
        const std::string token = login(name);
        pins.setPin(token, kPin);
        return token;
    }
    std::string deposit(const std::string& token, const char* amount) {
        const auto record = wallets.deposit(token, rupees(amount));
        clock.advance(1min);
        return record.reference();
    }
    std::string pay(const std::string& token, const std::string& payee, const char* amount) {
        const UPIPayment payment(payee, rupees(amount), kPin);
        const auto result = payments.pay(token, payment, PaymentService::newIdempotencyKey());
        clock.advance(1min);
        return result.transaction.reference();
    }
};

// A fixed scenario, one minute apart starting 2026-10-14T10:00:00Z:
//   10:00 asha deposits 1000              10:04 ravi deposits 100
//   10:01 asha -> ravi 250                10:05 ravi -> asha 40
//   10:02 asha -> ravi 5000 (FAILED: funds) 10:06 ravi -> meera 20
//   10:03 asha -> nobody 10 (FAILED: no such recipient)
// Final balances: asha 790, ravi 290, meera 20.
struct Seeded : AppFixture {
    std::string asha, ravi, meera;
    std::string ashaDeposit, ashaToRavi, ashaToRaviFailed, ashaToNobody, raviDeposit, raviToAsha, raviToMeera;

    explicit Seeded(const std::string& path = ":memory:") : AppFixture(path) {
        asha = signUp("asha", "9876543210");
        ravi = signUp("ravi", "9123456789");
        meera = signUp("meera", "9000000003");
        ashaDeposit = deposit(asha, "1000");
        ashaToRavi = pay(asha, "ravi@paysphere", "250");
        ashaToRaviFailed = pay(asha, "ravi@paysphere", "5000");
        ashaToNobody = pay(asha, "nobody@paysphere", "10");
        raviDeposit = deposit(ravi, "100");
        raviToAsha = pay(ravi, "asha@paysphere", "40");
        raviToMeera = pay(ravi, "meera@paysphere", "20");
    }

    std::vector<std::string> references(const std::string& token, const HistoryQuery& query = {}) {
        std::vector<std::string> result;
        for (const HistoryEntry& entry : transactions.history(token, query).entries) {
            result.push_back(entry.transaction.reference());
        }
        return result;
    }
};

using References = std::vector<std::string>;

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

// ------------------------------------------------------------ the model

TEST(transaction_model_knows_who_may_see_it) {
    const Transaction transfer(1, "T1", TransactionType::Transfer, TransactionStatus::Success, 10, 20, rupees("5"));
    const Transaction failedTransfer(2, "T2", TransactionType::Transfer, TransactionStatus::Failed, 10, 20,
                                     rupees("5"), "Incorrect UPI PIN");
    const Transaction failedDeposit(3, "D1", TransactionType::Deposit, TransactionStatus::Failed, std::nullopt, 20,
                                    rupees("5"), "Over the limit");

    CHECK(transfer.isVisibleTo(10) && transfer.isVisibleTo(20) && !transfer.isVisibleTo(30));
    CHECK(failedTransfer.isVisibleTo(10));  // the payer sees their failed attempt
    CHECK(!failedTransfer.isVisibleTo(20)); // the intended payee does not
    CHECK(failedDeposit.isVisibleTo(20));   // your own deposit, even if it failed

    CHECK(transfer.directionFor(10) == TransactionDirection::Sent);
    CHECK(transfer.directionFor(20) == TransactionDirection::Received);
    CHECK(failedDeposit.directionFor(20) == TransactionDirection::Received);
    CHECK_THROWS_AS(transfer.directionFor(30), ValidationError);
}

TEST(timestamps_are_shown_in_indian_standard_time) {
    CHECK_EQ(formatIndianTime(parseTimestamp("2026-10-14T10:01:00Z")), "14 Oct 2026, 03:31:00 PM IST");
    CHECK_EQ(formatIndianTime(parseTimestamp("2026-10-14T06:30:00Z")), "14 Oct 2026, 12:00:00 PM IST");
    CHECK_EQ(formatIndianTime(parseTimestamp("2026-12-31T18:45:09Z")), "01 Jan 2027, 12:15:09 AM IST");
}

// ----------------------------------------------------------------- history

TEST(history_lists_deposits_and_transfers_newest_first) {
    Seeded s;
    const HistoryPage page = s.transactions.history(s.asha);
    CHECK_EQ(page.totalMatching, 5);
    CHECK(s.references(s.asha) ==
          (References{s.raviToAsha, s.ashaToNobody, s.ashaToRaviFailed, s.ashaToRavi, s.ashaDeposit}));

    const HistoryEntry& received = page.entries[0];
    CHECK(received.direction == TransactionDirection::Received);
    CHECK_EQ(received.counterparty->upiId, "ravi@paysphere");
    CHECK_EQ(received.counterparty->fullName, "Ravi Test");
    CHECK_EQ(received.transaction.createdAt(), "2026-10-14T10:05:00Z");

    const HistoryEntry& toNobody = page.entries[1];
    CHECK(toNobody.direction == TransactionDirection::Sent);
    CHECK(!toNobody.counterparty.has_value());
    CHECK_EQ(toNobody.transaction.failureReason(), PaymentService::kRecipientNotFound);

    const HistoryEntry& deposit = page.entries[4];
    CHECK(deposit.transaction.type() == TransactionType::Deposit);
    CHECK(deposit.direction == TransactionDirection::Received);
    CHECK(!deposit.counterparty.has_value());

    // Timestamps strictly decrease down the page.
    for (std::size_t i = 1; i < page.entries.size(); ++i) {
        CHECK(page.entries[i - 1].transaction.createdAt() > page.entries[i].transaction.createdAt());
    }
}

TEST(history_shows_only_the_callers_own_records) {
    Seeded s;
    // Ravi sees money he received from asha, but not asha's failed attempt to
    // pay him, nor asha's deposit.
    CHECK(s.references(s.ravi) == (References{s.raviToMeera, s.raviToAsha, s.raviDeposit, s.ashaToRavi}));
    // Meera sees only the payment she received.
    CHECK(s.references(s.meera) == (References{s.raviToMeera}));
    // And asha never sees ravi's payment to meera or ravi's deposit.
    for (const std::string& reference : s.references(s.asha)) {
        CHECK(reference != s.raviToMeera && reference != s.raviDeposit);
    }
}

TEST(history_filters_by_direction_and_status) {
    Seeded s;
    HistoryQuery sent;
    sent.direction = TransactionDirection::Sent;
    CHECK(s.references(s.asha, sent) == (References{s.ashaToNobody, s.ashaToRaviFailed, s.ashaToRavi}));

    HistoryQuery received;
    received.direction = TransactionDirection::Received;
    CHECK(s.references(s.asha, received) == (References{s.raviToAsha, s.ashaDeposit}));

    HistoryQuery failed;
    failed.status = TransactionStatus::Failed;
    CHECK(s.references(s.asha, failed) == (References{s.ashaToNobody, s.ashaToRaviFailed}));
    CHECK_EQ(s.transactions.history(s.asha, failed).totalMatching, 2);

    HistoryQuery sentAndSucceeded = sent;
    sentAndSucceeded.status = TransactionStatus::Success;
    CHECK(s.references(s.asha, sentAndSucceeded) == (References{s.ashaToRavi}));

    HistoryQuery ravisFailures;
    ravisFailures.status = TransactionStatus::Failed;
    CHECK(s.references(s.ravi, ravisFailures).empty()); // asha's failure is not his
}

TEST(history_paginates_and_validates_page_bounds) {
    Seeded s;
    HistoryQuery query;
    query.limit = 2;
    References all;
    for (query.offset = 0; query.offset < 6; query.offset += 2) {
        const HistoryPage page = s.transactions.history(s.asha, query);
        CHECK_EQ(page.totalMatching, 5);
        for (const auto& entry : page.entries) {
            all.push_back(entry.transaction.reference());
        }
    }
    CHECK(all == s.references(s.asha)); // pages join up with no gaps or repeats

    query.offset = 50;
    CHECK(s.transactions.history(s.asha, query).entries.empty());

    for (const auto& [limit, offset] : {std::pair<std::int64_t, std::int64_t>{0, 0}, {101, 0}, {10, -1}}) {
        HistoryQuery bad;
        bad.limit = limit;
        bad.offset = offset;
        CHECK_THROWS_AS(s.transactions.history(s.asha, bad), ValidationError);
    }
}

TEST(history_requires_a_live_session) {
    Seeded s;
    CHECK_THROWS_AS(s.transactions.history("forged-token"), SessionError);
    s.auth.logout(s.asha);
    CHECK_THROWS_AS(s.transactions.history(s.asha), SessionError);
    CHECK_THROWS_AS(s.transactions.findByReference(s.asha, s.ashaToRavi), SessionError);
    CHECK_THROWS_AS(s.transactions.receipt(s.asha, s.ashaToRavi), SessionError);
    CHECK_THROWS_AS(s.transactions.audit(s.asha), SessionError);
}

// ------------------------------------------------------------------ search

TEST(search_finds_a_transaction_by_id_for_both_parties) {
    Seeded s;
    std::string typed = s.ashaToRavi;
    for (char& c : typed) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    const auto forAsha = s.transactions.findByReference(s.asha, "  " + typed + " ");
    CHECK(forAsha.has_value());
    CHECK_EQ(forAsha->transaction.reference(), s.ashaToRavi);
    CHECK(forAsha->direction == TransactionDirection::Sent);
    CHECK_EQ(forAsha->counterparty->upiId, "ravi@paysphere");

    const auto forRavi = s.transactions.findByReference(s.ravi, s.ashaToRavi);
    CHECK(forRavi.has_value());
    CHECK(forRavi->direction == TransactionDirection::Received);
    CHECK_EQ(forRavi->counterparty->upiId, "asha@paysphere");
}

TEST(search_hides_other_users_records_like_unknown_ids) {
    Seeded s;
    CHECK(!s.transactions.findByReference(s.meera, s.ashaToRavi).has_value());      // not a party
    CHECK(!s.transactions.findByReference(s.asha, s.raviToMeera).has_value());      // not a party
    CHECK(!s.transactions.findByReference(s.ravi, s.ashaToRaviFailed).has_value()); // failed incoming
    CHECK(!s.transactions.findByReference(s.asha, "TRF20261014FFFFFFFFFFFF").has_value());
    CHECK(!s.transactions.findByReference(s.asha, "").has_value());
    CHECK(!s.transactions.findByReference(s.asha, std::string(1000, 'A')).has_value());
    CHECK(!s.transactions.findByReference(s.asha, "' OR '1'='1").has_value());
}

// ---------------------------------------------------------------- receipts

TEST(receipt_for_a_successful_transfer) {
    Seeded s;
    const std::string text = s.transactions.receipt(s.asha, s.ashaToRavi).toText();
    CHECK(contains(text, "Simulated transaction - no real money moved"));
    CHECK(contains(text, "Transaction ID  " + s.ashaToRavi + "\n"));
    CHECK(contains(text, "Type            UPI transfer (sent)\n"));
    CHECK(contains(text, "Status          SUCCESS\n"));
    CHECK(contains(text, "Amount          Rs. 250.00\n"));
    CHECK(contains(text, "From            Asha Test <asha@paysphere>\n"));
    CHECK(contains(text, "To              Ravi Test <ravi@paysphere>\n"));
    CHECK(contains(text, "Date            14 Oct 2026, 03:31:00 PM IST\n"));
    CHECK(contains(text, "UTC             2026-10-14T10:01:00Z\n"));
    CHECK(!contains(text, "Reason"));

    const std::string ravisCopy = s.transactions.receipt(s.ravi, s.ashaToRavi).toText();
    CHECK(contains(ravisCopy, "Type            UPI transfer (received)\n"));
    CHECK(contains(ravisCopy, "Amount          Rs. 250.00\n"));
}

TEST(receipts_for_failures_and_deposits) {
    Seeded s;
    const std::string failed = s.transactions.receipt(s.asha, s.ashaToRaviFailed).toText();
    CHECK(contains(failed, "Status          FAILED\n"));
    CHECK(contains(failed, "Reason          Insufficient balance\n"));

    const std::string unknown = s.transactions.receipt(s.asha, s.ashaToNobody).toText();
    // Audit fix (schema v6): the attempted payee is kept even though no such user exists.
    CHECK(contains(unknown, "To              (not found) nobody@paysphere\n"));
    CHECK(contains(unknown, "Reason          Recipient UPI ID not found\n"));

    const std::string deposit = s.transactions.receipt(s.asha, s.ashaDeposit).toText();
    CHECK(contains(deposit, "Type            Wallet deposit\n"));
    CHECK(contains(deposit, "From            Simulated bank deposit\n"));
    CHECK(contains(deposit, "To              Asha Test <asha@paysphere>\n"));
}

TEST(receipts_are_refused_to_non_parties) {
    Seeded s;
    CHECK_THROWS_AS(s.transactions.receipt(s.meera, s.ashaToRavi), AuthorizationError);
    CHECK_THROWS_AS(s.transactions.receipt(s.ravi, s.ashaToRaviFailed), AuthorizationError);
    CHECK_THROWS_AS(s.transactions.receipt(s.asha, s.raviDeposit), AuthorizationError);
    CHECK_THROWS_AS(s.transactions.receipt(s.asha, "DEP20261014000000000000"), AuthorizationError);
}

// ------------------------------------------- integrity and auditability

TEST(statuses_are_consistent_and_records_cannot_be_modified) {
    Seeded s;
    for (const std::string& token : {s.asha, s.ravi, s.meera}) {
        for (const HistoryEntry& entry : s.transactions.history(token).entries) {
            CHECK_EQ(entry.transaction.succeeded(), entry.transaction.failureReason().empty());
        }
        CHECK(s.transactions.audit(token).consistent());
    }

    // Not even raw SQL can rewrite or erase history.
    CHECK_THROWS_AS(s.db.execute("UPDATE transactions SET status = 'SUCCESS', failure_reason = NULL "
                                 "WHERE reference = ?",
                                 {s.ashaToRaviFailed}),
                    ConstraintViolation);
    CHECK_THROWS_AS(s.db.execute("UPDATE transactions SET amount_paise = 1 WHERE reference = ?", {s.ashaToRavi}),
                    ConstraintViolation);
    CHECK_THROWS_AS(s.db.execute("DELETE FROM transactions WHERE reference = ?", {s.ashaToRavi}),
                    ConstraintViolation);
    // A deposit has no idempotency key whose foreign key could block the
    // delete, so this checks the append-only trigger on its own.
    CHECK_THROWS_AS(s.db.execute("DELETE FROM transactions WHERE reference = ?", {s.ashaDeposit}),
                    ConstraintViolation);
    CHECK(s.transactions.findByReference(s.asha, s.ashaDeposit).has_value());
    CHECK_EQ(s.transactions.findByReference(s.asha, s.ashaToRaviFailed)->transaction.failureReason(),
             PaymentService::kInsufficientFunds);
    CHECK_EQ(s.transactions.history(s.asha).totalMatching, 5);
}

TEST(deposits_and_transfers_are_reflected_in_balances_and_ledgers) {
    Seeded s;
    CHECK_EQ(s.wallets.balance(s.asha), rupees("790"));
    CHECK_EQ(s.wallets.balance(s.ravi), rupees("290"));
    CHECK_EQ(s.wallets.balance(s.meera), rupees("20"));
    const auto audit = s.transactions.audit(s.ravi);
    CHECK_EQ(audit.storedBalance, rupees("290"));
    CHECK_EQ(audit.ledgerBalance, rupees("290"));
}

TEST(audit_detects_a_balance_changed_without_a_record) {
    Seeded s;
    CHECK(s.transactions.audit(s.asha).consistent());
    s.db.execute("UPDATE wallets SET balance_paise = balance_paise + 10000 "
                 "WHERE user_id = (SELECT id FROM users WHERE upi_id = 'asha@paysphere')");
    const auto audit = s.transactions.audit(s.asha);
    CHECK(!audit.consistent());
    CHECK_EQ(audit.storedBalance, rupees("890"));
    CHECK_EQ(audit.ledgerBalance, rupees("790"));
}

// ------------------------------------------------------------ persistence

TEST(history_and_receipts_survive_an_application_restart) {
    TempDirectory directory;
    const std::string path = directory.file("paysphere.db");
    References before;
    std::string receiptBefore;
    std::string ashaToRavi;
    {
        Seeded first(path);
        before = first.references(first.asha);
        receiptBefore = first.transactions.receipt(first.asha, first.ashaToRavi).toText();
        ashaToRavi = first.ashaToRavi;
    } // shut down: in-memory sessions are gone

    AppFixture second(path);
    const std::string asha = second.login("asha");
    CHECK(second.transactions.history(asha).totalMatching == 5);
    References after;
    for (const auto& entry : second.transactions.history(asha).entries) {
        after.push_back(entry.transaction.reference());
    }
    CHECK(after == before);
    CHECK_EQ(second.transactions.receipt(asha, ashaToRavi).toText(), receiptBefore);
    CHECK(second.transactions.audit(asha).consistent());

    // Still append-only after reopening, and new activity is added on top.
    CHECK_THROWS_AS(second.db.execute("DELETE FROM transactions"), ConstraintViolation);
    const std::string newer = second.pay(asha, "ravi@paysphere", "1");
    CHECK_EQ(second.transactions.history(asha).entries.front().transaction.reference(), newer);
    CHECK_EQ(second.transactions.history(asha).totalMatching, 6);
}
