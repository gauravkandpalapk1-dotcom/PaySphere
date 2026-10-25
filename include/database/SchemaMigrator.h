#pragma once

namespace paysphere::db {

class Database;

// Creates and upgrades the database schema.
//
// The schema version is stored in SQLite's built-in PRAGMA user_version.
// Migrations are numbered and append-only: a released migration is never
// edited; later phases add new ones and raise kLatestVersion.
class SchemaMigrator {
public:
    static constexpr int kLatestVersion = 6;

    explicit SchemaMigrator(Database& database);

    int currentVersion() const;

    // Applies pending migrations up to targetVersion in a single transaction
    // and returns how many ran. Throws DatabaseError if the file comes from a
    // newer build. A lower target is mainly for testing upgrade paths.
    int migrate(int targetVersion = kLatestVersion);

private:
    Database& database_;
};

} // namespace paysphere::db
