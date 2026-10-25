#include "services/SessionManager.h"

#include "Sodium.h"
#include "models/Errors.h"

#include <sodium.h>

#include <algorithm>
#include <array>

namespace paysphere::services {

namespace {

constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kTokenHexLength = kTokenBytes * 2;
constexpr const char* kInvalidSession = "Your session is invalid or has expired; please log in again";

using detail::toHex;

} // namespace

SessionManager::SessionManager(const Clock& clock, SessionPolicy policy)
    : clock_(clock), policy_(policy) {
    detail::ensureSodiumInitialized();
    if (policy_.idleTimeout.count() <= 0 || policy_.absoluteTimeout < policy_.idleTimeout) {
        throw ValidationError("Session timeouts must be positive, and absolute >= idle");
    }
}

bool SessionManager::isWellFormed(const std::string& token) {
    return token.size() == kTokenHexLength &&
           std::all_of(token.begin(), token.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string SessionManager::hashToken(const std::string& token) {
    std::array<unsigned char, crypto_generichash_BYTES> digest{};
    crypto_generichash(digest.data(), digest.size(),
                       reinterpret_cast<const unsigned char*>(token.data()), token.size(), nullptr, 0);
    return toHex(digest.data(), digest.size());
}

TimePoint SessionManager::expiryOf(const Entry& entry) const {
    return std::min(entry.lastActivityAt + policy_.idleTimeout,
                    entry.createdAt + policy_.absoluteTimeout);
}

void SessionManager::purgeExpiredLocked(TimePoint now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = now >= expiryOf(it->second) ? sessions_.erase(it) : std::next(it);
    }
}

std::string SessionManager::create(std::int64_t userId) {
    if (userId <= 0) {
        throw ValidationError("A session must belong to a saved user");
    }
    std::array<unsigned char, kTokenBytes> random{};
    randombytes_buf(random.data(), random.size());
    std::string token = toHex(random.data(), random.size());
    sodium_memzero(random.data(), random.size());

    const TimePoint now = clock_.now();
    const std::lock_guard<std::mutex> lock(mutex_);
    purgeExpiredLocked(now);
    sessions_.insert_or_assign(hashToken(token), Entry{userId, now, now});
    return token;
}

models::Session SessionManager::validate(const std::string& token) {
    if (!isWellFormed(token)) {
        throw SessionError(kInvalidSession);
    }
    const std::string key = hashToken(token);
    const TimePoint now = clock_.now();

    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(key);
    if (it == sessions_.end()) {
        throw SessionError(kInvalidSession);
    }
    if (now >= expiryOf(it->second)) {
        sessions_.erase(it);
        throw SessionError(kInvalidSession);
    }
    it->second.lastActivityAt = now;
    return models::Session(it->second.userId, it->second.createdAt, now, expiryOf(it->second));
}

bool SessionManager::revoke(const std::string& token) {
    if (!isWellFormed(token)) {
        return false;
    }
    const std::string key = hashToken(token);
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.erase(key) > 0;
}

std::size_t SessionManager::revokeAllForUser(std::int64_t userId) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.userId == userId) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t SessionManager::activeCount() {
    const TimePoint now = clock_.now();
    const std::lock_guard<std::mutex> lock(mutex_);
    purgeExpiredLocked(now);
    return sessions_.size();
}

} // namespace paysphere::services
