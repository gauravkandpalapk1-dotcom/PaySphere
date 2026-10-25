#include "TestFramework.h"

#include "models/Errors.h"
#include "models/Transaction.h"
#include "models/User.h"
#include "models/Wallet.h"

#include <optional>

using paysphere::PaySphereError;
using paysphere::ValidationError;
using paysphere::models::parseTransactionStatus;
using paysphere::models::parseTransactionType;
using paysphere::models::Transaction;
using paysphere::models::TransactionStatus;
using paysphere::models::TransactionType;
using paysphere::models::User;
using paysphere::models::Wallet;
using paysphere::payments::Money;

namespace {

const Money kHundred = Money::fromPaise(10000);

} // namespace

TEST(user_holds_its_fields) {
    const User user(7, "Asha Verma", "asha@example.com", "9876543210", "asha@paysphere");
    CHECK_EQ(user.id(), 7);
    CHECK(user.isPersisted());
    CHECK_EQ(user.fullName(), "Asha Verma");
    CHECK_EQ(user.email(), "asha@example.com");
    CHECK_EQ(user.upiId(), "asha@paysphere");
    CHECK(!User(0, "New User", "new@example.com", "9876543210", "new@paysphere").isPersisted());
}

TEST(user_rejects_missing_fields) {
    CHECK_THROWS_AS(User(-1, "A", "a@example.com", "9876543210", "a@paysphere"), ValidationError);
    CHECK_THROWS_AS(User(1, "", "a@example.com", "9876543210", "a@paysphere"), ValidationError);
    // An empty email is allowed: accounts created before schema v3 have none.
    CHECK_EQ(User(1, "A", "", "9876543210", "a@paysphere").email(), "");
    CHECK_THROWS_AS(User(1, "A", "a@example.com", "", "a@paysphere"), ValidationError);
    CHECK_THROWS_AS(User(1, "A", "a@example.com", "9876543210", ""), ValidationError);
}

TEST(wallet_rejects_negative_balance_and_missing_owner) {
    const Wallet wallet(1, 2, kHundred);
    CHECK_EQ(wallet.balance(), kHundred);
    CHECK_THROWS_AS(Wallet(1, 2, Money::fromPaise(-1)), ValidationError);
    CHECK_THROWS_AS(Wallet(1, 0, kHundred), ValidationError);
}

TEST(transaction_accepts_valid_deposit_and_transfers) {
    const Transaction deposit(1, "PS1", TransactionType::Deposit, TransactionStatus::Success,
                              std::nullopt, 5, kHundred);
    CHECK(deposit.succeeded());

    const Transaction transfer(2, "PS2", TransactionType::Transfer, TransactionStatus::Success,
                               5, 6, kHundred);
    CHECK_EQ(*transfer.toWalletId(), 6);

    // A transfer to an unknown recipient fails before a destination exists.
    const Transaction failed(3, "PS3", TransactionType::Transfer, TransactionStatus::Failed,
                             5, std::nullopt, kHundred, "Recipient not found");
    CHECK(!failed.succeeded());
    CHECK_EQ(failed.failureReason(), "Recipient not found");
}

TEST(transaction_enforces_invariants) {
    const auto success = TransactionStatus::Success;
    const auto deposit = TransactionType::Deposit;
    const auto transfer = TransactionType::Transfer;

    CHECK_THROWS_AS(Transaction(1, "", deposit, success, std::nullopt, 5, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", deposit, success, std::nullopt, 5, Money::zero()), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", deposit, success, 4, 5, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", transfer, success, std::nullopt, 5, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", transfer, success, 5, std::nullopt, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", transfer, success, 5, 5, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", transfer, TransactionStatus::Failed, 5, 6, kHundred), ValidationError);
    CHECK_THROWS_AS(Transaction(1, "R", transfer, success, 5, 6, kHundred, "reason"), ValidationError);
}

TEST(transaction_enum_text_round_trips) {
    CHECK(parseTransactionType(toString(TransactionType::Deposit)) == TransactionType::Deposit);
    CHECK(parseTransactionType(toString(TransactionType::Transfer)) == TransactionType::Transfer);
    CHECK(parseTransactionStatus(toString(TransactionStatus::Success)) == TransactionStatus::Success);
    CHECK(parseTransactionStatus(toString(TransactionStatus::Failed)) == TransactionStatus::Failed);
    CHECK_THROWS_AS(parseTransactionType("deposit"), ValidationError);
    CHECK_THROWS_AS(parseTransactionStatus("PENDING"), ValidationError);
}

TEST(errors_report_their_category_polymorphically) {
    try {
        throw ValidationError("bad input");
    } catch (const PaySphereError& error) {
        CHECK_EQ(std::string(error.code()), "VALIDATION_ERROR");
        CHECK_EQ(std::string(error.what()), "bad input");
    }
}
