#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace paysphere::db {

// A value bound to a "?" placeholder. The schema stores only integers
// (ids, paise, flags) and text, so no floating-point alternative is offered:
// money must be passed as integer paise, e.g. Money::paise().
using SqlValue = std::variant<std::nullptr_t, std::int64_t, std::string>;

// Positional parameters: element 0 binds to the first "?".
using SqlParams = std::vector<SqlValue>;

} // namespace paysphere::db
