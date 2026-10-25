#pragma once

#include <string>

namespace paysphere::web {

// Escapes text for safe use inside HTML element content and inside quoted
// attribute values: & < > " ' become character references. Every piece of
// user-controlled text shown in a page goes through this, which is what
// stops HTML or script injection (XSS).
std::string escapeHtml(const std::string& text);

} // namespace paysphere::web
