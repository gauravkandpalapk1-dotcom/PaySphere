#pragma once

#include "models/Session.h"
#include "services/Clock.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace paysphere::services {

struct SessionPolicy {
    // Signed out after this long without activity...
    std::chrono::seconds idleTimeout{std::chrono::minutes(30)};
    // ...and after this long in total, however active.
    std::chrono::seconds absoluteTimeout{std::chrono::hours(12)};
};

// Issues and checks session tokens for authenticated users.
//
// A token is 32 bytes from libsodium's CSPRNG (256 bits, unguessable),
// handed to the client as 64 hex characters. Only a BLAKE2b hash of each
// token is kept, so a dump of this object's memory does not reveal usable
// tokens. Sessions live in memory: they end when the program exits, which
// suits a single-process simulator.
//
// Thread-safe.
class SessionManager {
public:
    explicit SessionManager(const Clock& clock, SessionPolicy policy = {});

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Starts a session and returns its token. This is the only time the
    // token is available; the caller must keep it secret.
    std::string create(std::int64_t userId);

    // Returns the session for a live token and refreshes its idle timer.
    // Throws SessionError if the token is malformed, unknown, logged out or
    // expired.
    models::Session validate(const std::string& token);

    // Ends one session (logout). Returns false if it was not active.
    bool revoke(const std::string& token);
    // Ends every session of a user, e.g. after a password change.
    std::size_t revokeAllForUser(std::int64_t userId);

    // Live sessions, after discarding expired ones.
    std::size_t activeCount();

    const SessionPolicy& policy() const noexcept { return policy_; }

private:
    struct Entry {
        std::int64_t userId;
        TimePoint createdAt;
        TimePoint lastActivityAt;
    };

    static bool isWellFormed(const std::string& token);
    static std::string hashToken(const std::string& token);
    TimePoint expiryOf(const Entry& entry) const;
    void purgeExpiredLocked(TimePoint now);

    const Clock& clock_;
    SessionPolicy policy_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> sessions_; // key: hash of the token
};

} // namespace paysphere::services
