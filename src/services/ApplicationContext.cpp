#include "services/ApplicationContext.h"

namespace paysphere::services {

ApplicationContext::ApplicationContext(const std::string& databasePath, ApplicationOptions options)
    : database_(databasePath, options.busyTimeout),
      clock_(options.clock != nullptr ? *options.clock : systemClock_),
      passwordHasher_(options.passwordCost),
      sessions_(clock_),
      auth_(database_, sessions_, passwordHasher_, clock_),
      wallets_(database_, auth_, references_, clock_),
      pins_(database_, auth_, passwordHasher_, clock_),
      risk_(database_, clock_),
      // Incoming transfers obey the same wallet cap as deposits.
      payments_(database_, auth_, pins_, risk_, references_, clock_,
                PaymentLimits{PaymentLimits{}.minAmount, PaymentLimits{}.maxAmount, wallets_.limits().maxBalance}),
      transactions_(database_, auth_) {
    database_.initialize();
}

} // namespace paysphere::services
