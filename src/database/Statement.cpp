#include "database/Statement.h"

#include "SqliteError.h"
#include "database/Database.h"
#include "database/DatabaseError.h"

#include <sqlite3.h>

#include <cctype>
#include <limits>
#include <utility>
#include <variant>

namespace paysphere::db {

namespace {

bool onlyWhitespaceOrSemicolons(const char* text) {
    for (; *text != '\0'; ++text) {
        const auto c = static_cast<unsigned char>(*text);
        if (c != ';' && std::isspace(c) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

Statement::Statement(Database& database, const std::string& sql)
    : db_(database.requireOpen()) {
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, &tail);
    if (rc != SQLITE_OK) {
        detail::throwLastError(db_, rc, "Cannot prepare SQL");
    }
    if (stmt_ == nullptr) {
        throw DatabaseError("SQL text contains no statement", SQLITE_MISUSE);
    }
    // Refuse trailing statements: they would be silently ignored.
    if (tail != nullptr && !onlyWhitespaceOrSemicolons(tail)) {
        finalize(); // the destructor does not run when a constructor throws
        throw DatabaseError("prepare() accepts exactly one SQL statement", SQLITE_MISUSE);
    }
}

Statement::~Statement() {
    finalize();
}

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        finalize();
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

void Statement::finalize() noexcept {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

sqlite3_stmt* Statement::requireValid() const {
    if (stmt_ == nullptr) {
        throw DatabaseError("Statement has been moved from", SQLITE_MISUSE);
    }
    return stmt_;
}

void Statement::checkBind(int resultCode, int index) const {
    if (resultCode != SQLITE_OK) {
        detail::throwLastError(db_, resultCode, "Cannot bind parameter " + std::to_string(index));
    }
}

Statement& Statement::bind(int index, std::int64_t value) {
    checkBind(sqlite3_bind_int64(requireValid(), index, value), index);
    return *this;
}

Statement& Statement::bind(int index, const std::string& value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw DatabaseError("Bound text is too large", SQLITE_TOOBIG);
    }
    // SQLITE_TRANSIENT makes SQLite copy the text, so value may be a temporary.
    checkBind(sqlite3_bind_text(requireValid(), index, value.data(),
                                static_cast<int>(value.size()), SQLITE_TRANSIENT),
              index);
    return *this;
}

Statement& Statement::bindNull(int index) {
    checkBind(sqlite3_bind_null(requireValid(), index), index);
    return *this;
}

Statement& Statement::bindValue(int index, const SqlValue& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return bindNull(index);
    }
    if (const auto* number = std::get_if<std::int64_t>(&value)) {
        return bind(index, *number);
    }
    return bind(index, std::get<std::string>(value));
}

Statement& Statement::bindAll(const SqlParams& params) {
    const int expected = sqlite3_bind_parameter_count(requireValid());
    if (params.size() != static_cast<std::size_t>(expected)) {
        throw DatabaseError("SQL expects " + std::to_string(expected) + " parameter(s) but " +
                                std::to_string(params.size()) + " were supplied",
                            SQLITE_RANGE);
    }
    for (int i = 0; i < expected; ++i) {
        bindValue(i + 1, params[static_cast<std::size_t>(i)]);
    }
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(requireValid());
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    detail::throwLastError(db_, rc, "SQL execution failed");
}

void Statement::reset() {
    sqlite3_stmt* stmt = requireValid();
    // reset() repeats the last step() error, which was already reported.
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

int Statement::columnCount() const {
    return sqlite3_column_count(requireValid());
}

void Statement::checkColumn(int column, int expectedType, const char* typeName) const {
    sqlite3_stmt* stmt = requireValid();
    // data_count is 0 unless step() has just returned a row.
    if (column < 0 || column >= sqlite3_data_count(stmt)) {
        throw DatabaseError("Column " + std::to_string(column) + " is not available", SQLITE_RANGE);
    }
    if (expectedType != SQLITE_NULL && sqlite3_column_type(stmt, column) != expectedType) {
        throw DatabaseError("Column " + std::to_string(column) + " does not hold " + typeName,
                            SQLITE_MISMATCH);
    }
}

bool Statement::columnIsNull(int column) const {
    checkColumn(column, SQLITE_NULL, "any value");
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

std::int64_t Statement::columnInt64(int column) const {
    checkColumn(column, SQLITE_INTEGER, "an integer");
    return sqlite3_column_int64(stmt_, column);
}

std::string Statement::columnText(int column) const {
    checkColumn(column, SQLITE_TEXT, "text");
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    const int bytes = sqlite3_column_bytes(stmt_, column);
    if (text == nullptr) {
        throw DatabaseError("Out of memory reading column " + std::to_string(column), SQLITE_NOMEM);
    }
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

} // namespace paysphere::db
