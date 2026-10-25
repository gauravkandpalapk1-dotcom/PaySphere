#pragma once

#include "database/SqlValue.h"

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace paysphere::db {

class Database;

// A compiled SQL statement (RAII: finalized in the destructor). Values are
// passed through bind() rather than spliced into the SQL text, which rules
// out SQL injection.
//
// Parameter and column indices follow SQLite: bind() is 1-based, column
// accessors are 0-based. Column accessors check the stored type, so a
// corrupted or mistyped value raises an error instead of silently reading 0.
class Statement {
public:
    Statement(Database& database, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, const std::string& value);
    Statement& bindNull(int index);
    Statement& bindValue(int index, const SqlValue& value);
    // Binds params to placeholders 1..n. The count must match the SQL
    // exactly: a forgotten parameter would otherwise silently become NULL.
    Statement& bindAll(const SqlParams& params);

    // Advances the statement: true when a result row is available, false when done.
    bool step();
    // Rewinds the statement and clears bindings so it can be reused.
    void reset();

    int columnCount() const;
    bool columnIsNull(int column) const;
    std::int64_t columnInt64(int column) const;
    std::string columnText(int column) const;

private:
    sqlite3_stmt* requireValid() const;
    void checkColumn(int column, int expectedType, const char* typeName) const;
    void checkBind(int resultCode, int index) const;
    void finalize() noexcept;

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace paysphere::db
