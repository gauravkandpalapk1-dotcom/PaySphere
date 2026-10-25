#pragma once

#include <memory>
#include <string>

namespace paysphere::services {
class ApplicationContext;
}

namespace paysphere::web {

// The browser front end: HTML pages (from web/templates) and CSS (from
// web/static), served by an HTTP server on this computer only.
//
// Like the CLI, it is presentation only: every action calls the same
// services through ApplicationContext, so all business rules (validation,
// authorization, limits, atomic payments, idempotency) are unchanged.
//
// Web-specific protections:
//   - listens on 127.0.0.1 only, so other computers cannot connect;
//   - the session token lives in an HttpOnly, SameSite=Strict cookie;
//   - every form carries a CSRF token that must match a cookie;
//   - every value shown in a page is HTML-escaped;
//   - pages are served with a strict Content-Security-Policy (no scripts
//     at all) and are never cached;
//   - each payment form carries its own idempotency key, so refreshing
//     after "Pay" cannot pay twice.
//
// Requests are handled one at a time (one database connection).
class WebApp {
public:
    // webRoot is the folder holding templates/ and static/.
    // Throws PaySphereError if the templates cannot be loaded.
    WebApp(services::ApplicationContext& app, const std::string& webRoot);
    ~WebApp();

    WebApp(const WebApp&) = delete;
    WebApp& operator=(const WebApp&) = delete;

    // Binds to 127.0.0.1:port; port 0 picks any free port. Returns the port.
    // Throws PaySphereError if the port cannot be used.
    int bind(int port);
    // Serves requests until stop() is called (blocks the calling thread).
    void serve();
    // Blocks until the server accepts connections (after serve() started).
    void waitUntilReady() const;
    void stop();

private:
    // Pimpl: keeps the HTTP library's large header out of every file that
    // includes this one.
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paysphere::web
