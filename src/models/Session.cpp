#include "models/Session.h"

#include "models/Errors.h"

namespace paysphere::models {

Session::Session(std::int64_t userId, TimePoint createdAt, TimePoint lastActivityAt,
                 TimePoint expiresAt)
    : userId_(userId),
      createdAt_(createdAt),
      lastActivityAt_(lastActivityAt),
      expiresAt_(expiresAt) {
    if (userId_ <= 0) {
        throw ValidationError("A session must belong to a saved user");
    }
    if (lastActivityAt_ < createdAt_ || expiresAt_ < lastActivityAt_) {
        throw ValidationError("Session times are out of order");
    }
}

} // namespace paysphere::models
