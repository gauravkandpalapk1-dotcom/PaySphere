#pragma once

namespace paysphere::db {

class Database;

// Scope guard for an atomic unit of work (RAII):
//
//     DbTransaction tx(db);   // BEGIN IMMEDIATE
//     ...writes...            // any exception leaves the scope early
//     tx.commit();            // COMMIT
//
// If commit() is not reached, the destructor rolls back, so a failure
// part-way through can never leave half-applied changes.
class DbTransaction {
public:
    enum class Mode {
        // BEGIN IMMEDIATE: takes the write lock up front, so a
        // read-check-write sequence (such as checking a balance and then
        // debiting it) cannot interleave with another writer.
        Write,
        // BEGIN DEFERRED: a consistent read snapshot without blocking writers
        // (in WAL mode). Writing inside it is not intended.
        Read,
    };

    explicit DbTransaction(Database& database, Mode mode = Mode::Write);
    ~DbTransaction();

    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&&) = delete;
    DbTransaction& operator=(DbTransaction&&) = delete;

    void commit();
    void rollback();
    bool isActive() const noexcept { return active_; }

private:
    void requireActive() const;

    Database& database_;
    bool active_ = false;
};

} // namespace paysphere::db
