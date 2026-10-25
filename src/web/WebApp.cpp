#include "web/WebApp.h"

#include "models/Errors.h"
#include "models/Transaction.h"
#include "models/User.h"
#include "payments/Money.h"
#include "payments/SecretString.h"
#include "payments/UPIPayment.h"
#include "services/ApplicationContext.h"
#include "services/Clock.h"
#include "web/Html.h"
#include "web/Template.h"

#include <httplib.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace paysphere::web {

namespace {

constexpr const char* kSessionCookie = "ps_session";
constexpr const char* kCsrfCookie = "ps_csrf";
constexpr const char* kHtml = "text/html; charset=utf-8";
constexpr std::size_t kCsrfLength = 32; // hex characters
constexpr std::int64_t kHistoryPageSize = 10;
constexpr std::int64_t kRecentCount = 5;

struct Notice {
    const char* kind; // "success", "error" or "info" (a CSS class suffix)
    std::string text;
};

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool isLowerHex(const std::string& text, std::size_t length) {
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// Compares two secrets in time that does not depend on where they differ.
bool sameSecret(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference = static_cast<unsigned char>(difference | (a[i] ^ b[i]));
    }
    return difference == 0;
}

std::string cookieValue(const httplib::Request& request, const std::string& name) {
    const std::string header = request.get_header_value("Cookie");
    std::size_t position = 0;
    while (position < header.size()) {
        std::size_t end = header.find(';', position);
        if (end == std::string::npos) {
            end = header.size();
        }
        const std::string pair = trim(header.substr(position, end - position));
        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos && pair.substr(0, equals) == name) {
            return pair.substr(equals + 1);
        }
        position = end + 1;
    }
    return {};
}

// HttpOnly: page scripts cannot read it. SameSite=Strict: the browser does
// not send it with requests that start on another site (blocks CSRF).
std::string cookie(const std::string& name, const std::string& value) {
    return name + "=" + value + "; Path=/; HttpOnly; SameSite=Strict";
}

std::string expiredCookie(const std::string& name) {
    return name + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
}

std::string noticeHtml(const std::optional<Notice>& notice) {
    if (!notice) {
        return {};
    }
    return "<div class=\"notice notice-" + std::string(notice->kind) + "\" role=\"status\">" +
           escapeHtml(notice->text) + "</div>";
}

std::string partyText(const std::optional<models::Party>& party) {
    return party ? party->fullName + " (" + party->upiId + ")" : std::string();
}

// Builds the transactions table. Every value is escaped.
std::string transactionTable(const std::vector<services::HistoryEntry>& entries) {
    if (entries.empty()) {
        return "<p class=\"muted\">No transactions yet.</p>";
    }
    std::string html =
        "<div class=\"table-wrap\"><table class=\"transactions\"><thead><tr>"
        "<th>Date (IST)</th><th>Details</th><th class=\"num\">Amount</th><th>Status</th>"
        "<th>Transaction ID</th></tr></thead><tbody>";
    for (const services::HistoryEntry& entry : entries) {
        const models::Transaction& t = entry.transaction;
        const bool sent = entry.direction == models::TransactionDirection::Sent;

        std::string details;
        if (t.type() == models::TransactionType::Deposit) {
            details = "Deposit (simulated)";
        } else if (sent) {
            details = "Paid to " + (entry.counterparty ? partyText(entry.counterparty)
                                                       : t.payeeUpiId() + " (not found)");
        } else {
            details = "Received from " + partyText(entry.counterparty);
        }

        html += t.succeeded() ? "<tr>" : "<tr class=\"failed\">";
        html += "<td>" + escapeHtml(services::formatIndianTime(services::parseTimestamp(t.createdAt()))) + "</td>";
        html += "<td>" + escapeHtml(details);
        if (!t.succeeded()) {
            html += "<span class=\"reason\">" + escapeHtml(t.failureReason()) + "</span>";
        }
        html += "</td>";
        html += std::string("<td class=\"num ") + (sent ? "debit\">&minus; " : "credit\">+ ") +
                escapeHtml(t.amount().toString()) + "</td>";
        html += t.succeeded() ? "<td><span class=\"badge badge-success\">SUCCESS</span></td>"
                              : "<td><span class=\"badge badge-failed\">FAILED</span></td>";
        html += "<td class=\"id\"><a href=\"/receipt?id=" + escapeHtml(t.reference()) + "\">" +
                escapeHtml(t.reference()) + "</a></td></tr>";
    }
    html += "</tbody></table></div>";
    return html;
}

// A positive page number from the query string; 1 if missing or invalid.
std::int64_t pageNumber(const std::string& text) {
    if (text.empty() || text.size() > 6 || text.find_first_not_of("0123456789") != std::string::npos) {
        return 1;
    }
    const std::int64_t page = std::stoll(text);
    return page < 1 ? 1 : page;
}

} // namespace

class WebApp::Impl {
public:
    Impl(services::ApplicationContext& app, const std::string& webRoot)
        : app_(app),
          layout_(Template::load(webRoot + "/templates/layout.html")),
          login_(Template::load(webRoot + "/templates/login.html")),
          register_(Template::load(webRoot + "/templates/register.html")),
          dashboard_(Template::load(webRoot + "/templates/dashboard.html")),
          payForm_(Template::load(webRoot + "/templates/pay_form.html")),
          pinForm_(Template::load(webRoot + "/templates/pin_form.html")),
          history_(Template::load(webRoot + "/templates/history.html")),
          receipt_(Template::load(webRoot + "/templates/receipt.html")),
          message_(Template::load(webRoot + "/templates/message.html")) {
        if (!server.set_mount_point("/static", webRoot + "/static")) {
            throw PaySphereError("Cannot serve static files from " + webRoot + "/static");
        }
        server.set_payload_max_length(64 * 1024);
        server.set_default_headers({
            // No scripts at all; styles and forms only from this server.
            {"Content-Security-Policy", "default-src 'none'; style-src 'self'; img-src 'self'; "
                                        "form-action 'self'; frame-ancestors 'none'; base-uri 'none'"},
            {"X-Content-Type-Options", "nosniff"},
            {"X-Frame-Options", "DENY"},
            {"Referrer-Policy", "no-referrer"},
            {"Cache-Control", "no-store"},
        });

        server.Get("/", wrap([](const httplib::Request&, httplib::Response& res, Visitor& visitor) {
            res.set_redirect(visitor.user ? "/dashboard" : "/login", httplib::StatusCode::SeeOther_303);
        }));
        server.Get("/login", wrap([this](auto& req, auto& res, auto& v) { showLogin(req, res, v); }));
        server.Post("/login", wrap([this](auto& req, auto& res, auto& v) { doLogin(req, res, v); }));
        server.Get("/register", wrap([this](auto& req, auto& res, auto& v) { showRegister(req, res, v); }));
        server.Post("/register", wrap([this](auto& req, auto& res, auto& v) { doRegister(req, res, v); }));
        server.Post("/logout", wrap([this](auto& req, auto& res, auto& v) { doLogout(req, res, v); }));
        server.Get("/dashboard", wrap([this](auto& req, auto& res, auto& v) { showDashboard(req, res, v); }));
        server.Post("/deposit", wrap([this](auto& req, auto& res, auto& v) { doDeposit(req, res, v); }));
        server.Post("/pay", wrap([this](auto& req, auto& res, auto& v) { doPay(req, res, v); }));
        server.Post("/pin", wrap([this](auto& req, auto& res, auto& v) { doSetPin(req, res, v); }));
        server.Get("/history", wrap([this](auto& req, auto& res, auto& v) { showHistory(req, res, v); }));
        server.Get("/receipt", wrap([this](auto& req, auto& res, auto& v) { showReceipt(req, res, v); }));

        // Pages for errors that no handler rendered (unknown URL, body too large...).
        server.set_error_handler([this](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty()) {
                return httplib::Server::HandlerResponse::Unhandled; // a handler already rendered it
            }
            const bool notFound = res.status == httplib::StatusCode::NotFound_404;
            res.set_content(simplePage(notFound ? "Page not found" : "Request not accepted",
                                       notFound ? "That page does not exist." : "The request could not be handled."),
                            kHtml);
            return httplib::Server::HandlerResponse::Handled;
        });
        // Last resort: never show internal details to the browser.
        server.set_exception_handler([this](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            res.status = httplib::StatusCode::InternalServerError_500;
            res.set_content(simplePage("Something went wrong", "Please try again."), kHtml);
        });
    }

    httplib::Server server;

private:
    // Who is making this request.
    struct Visitor {
        std::string csrf;         // token to embed in forms (matches the ps_csrf cookie)
        std::string sessionToken; // empty when not logged in
        std::optional<models::User> user;
        bool sessionExpired = false;
    };
    using PageHandler = std::function<void(const httplib::Request&, httplib::Response&, Visitor&)>;

    // Serializes requests (the services share one database connection),
    // identifies the visitor and turns exceptions into pages.
    httplib::Server::Handler wrap(PageHandler handler) {
        return [this, handler](const httplib::Request& req, httplib::Response& res) {
            const std::lock_guard<std::mutex> lock(mutex_);
            Visitor visitor = identify(req, res);
            try {
                handler(req, res, visitor);
            } catch (const SessionError&) {
                res.set_header("Set-Cookie", expiredCookie(kSessionCookie));
                res.set_redirect("/login?notice=expired", httplib::StatusCode::SeeOther_303);
            } catch (const std::exception&) {
                res.status = httplib::StatusCode::InternalServerError_500;
                res.set_content(simplePage("Something went wrong", "Please try again."), kHtml);
            }
        };
    }

    Visitor identify(const httplib::Request& req, httplib::Response& res) {
        Visitor visitor;
        visitor.csrf = cookieValue(req, kCsrfCookie);
        if (!isLowerHex(visitor.csrf, kCsrfLength)) {
            visitor.csrf = services::PaymentService::newIdempotencyKey(); // 128 random bits as hex
            res.set_header("Set-Cookie", cookie(kCsrfCookie, visitor.csrf));
        }
        const std::string token = cookieValue(req, kSessionCookie);
        if (!token.empty()) {
            try {
                visitor.user = app_.auth().authenticate(token);
                visitor.sessionToken = token;
            } catch (const SessionError&) {
                visitor.sessionExpired = true;
                res.set_header("Set-Cookie", expiredCookie(kSessionCookie));
            }
        }
        return visitor;
    }

    // Redirects to the login page unless the visitor is logged in.
    static bool requireUser(httplib::Response& res, const Visitor& visitor) {
        if (visitor.user) {
            return true;
        }
        res.set_redirect(visitor.sessionExpired ? "/login?notice=expired" : "/login?notice=signin",
                         httplib::StatusCode::SeeOther_303);
        return false;
    }

    // Every state-changing POST must carry the CSRF token from its own form,
    // and its fields must come from the body, not the URL.
    bool acceptForm(const httplib::Request& req, httplib::Response& res, const Visitor& visitor) {
        if (req.target.find('?') == std::string::npos && sameSecret(req.get_param_value("csrf"), visitor.csrf)) {
            return true;
        }
        res.status = httplib::StatusCode::Forbidden_403;
        sendPage(res, visitor, "Form expired",
                 message_.render({{"heading", "This form has expired"},
                                  {"text", "Please go back, reload the page and try again."},
                                  {"link", "/"},
                                  {"link_text", "Go to PaySphere"}}),
                 std::nullopt);
        return false;
    }

    std::string nav(const Visitor& visitor) const {
        if (!visitor.user) {
            return "<a href=\"/login\">Log in</a><a href=\"/register\">Register</a>";
        }
        return "<a href=\"/dashboard\">Dashboard</a><a href=\"/history\">History</a>"
               "<form method=\"post\" action=\"/logout\"><input type=\"hidden\" name=\"csrf\" value=\"" +
               escapeHtml(visitor.csrf) + "\"><button type=\"submit\" class=\"link-button\">Log out</button></form>";
    }

    void sendPage(httplib::Response& res, const Visitor& visitor, const std::string& title,
                  const std::string& content, const std::optional<Notice>& notice) {
        res.set_content(layout_.render({{"title", title},
                                        {"nav", nav(visitor)},
                                        {"notice", noticeHtml(notice)},
                                        {"content", content}}),
                        kHtml);
    }

    std::string simplePage(const std::string& heading, const std::string& text) const {
        return layout_.render({{"title", heading},
                               {"nav", "<a href=\"/\">Home</a>"},
                               {"notice", ""},
                               {"content", message_.render({{"heading", heading},
                                                            {"text", text},
                                                            {"link", "/"},
                                                            {"link_text", "Go to PaySphere"}})}});
    }

    // ------------------------------------------------------------ accounts

    void showLogin(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (visitor.user) {
            res.set_redirect("/dashboard", httplib::StatusCode::SeeOther_303);
            return;
        }
        // Only fixed messages: the URL selects one, it cannot inject text.
        std::optional<Notice> notice;
        const std::string code = req.get_param_value("notice");
        if (code == "registered") {
            notice = Notice{"success", "Account created. You can log in now."};
        } else if (code == "loggedout") {
            notice = Notice{"info", "You have been logged out."};
        } else if (code == "expired" || visitor.sessionExpired) {
            notice = Notice{"info", "Your session ended. Please log in again."};
        } else if (code == "signin") {
            notice = Notice{"info", "Please log in to continue."};
        }
        sendPage(res, visitor, "Log in", login_.render({{"csrf", visitor.csrf}, {"identifier", ""}}), notice);
    }

    void doLogin(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!acceptForm(req, res, visitor)) {
            return;
        }
        const std::string identifier = trim(req.get_param_value("identifier"));
        const payments::SecretString password(req.get_param_value("password"));
        try {
            const services::LoginResult result = app_.auth().login(identifier, password.value());
            if (!visitor.sessionToken.empty()) {
                app_.auth().logout(visitor.sessionToken); // replace, never stack, sessions
            }
            res.set_header("Set-Cookie", cookie(kSessionCookie, result.sessionToken));
            res.set_redirect("/dashboard", httplib::StatusCode::SeeOther_303);
        } catch (const PaySphereError& error) {
            res.status = httplib::StatusCode::Unauthorized_401;
            sendPage(res, visitor, "Log in",
                     login_.render({{"csrf", visitor.csrf}, {"identifier", identifier}}),
                     Notice{"error", error.what()});
        }
    }

    Template::Values registrationValues(const httplib::Request& req, const Visitor& visitor) const {
        return {{"csrf", visitor.csrf},
                {"full_name", req.get_param_value("full_name")},
                {"email", req.get_param_value("email")},
                {"phone", req.get_param_value("phone")},
                {"upi_id", req.get_param_value("upi_id")}};
    }

    void showRegister(const httplib::Request&, httplib::Response& res, Visitor& visitor) {
        sendPage(res, visitor, "Create an account",
                 register_.render({{"csrf", visitor.csrf}, {"full_name", ""}, {"email", ""}, {"phone", ""},
                                   {"upi_id", ""}}),
                 std::nullopt);
    }

    void doRegister(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!acceptForm(req, res, visitor)) {
            return;
        }
        const payments::SecretString password(req.get_param_value("password"));
        const payments::SecretString confirm(req.get_param_value("confirm"));
        std::optional<Notice> problem;
        if (password.value() != confirm.value()) {
            problem = Notice{"error", "The two passwords do not match."};
        } else {
            services::RegistrationRequest request{req.get_param_value("full_name"), req.get_param_value("email"),
                                                  req.get_param_value("phone"), req.get_param_value("upi_id"),
                                                  password.value()};
            try {
                app_.auth().registerUser(request);
                payments::secureWipe(request.password);
                res.set_redirect("/login?notice=registered", httplib::StatusCode::SeeOther_303);
                return;
            } catch (const PaySphereError& error) {
                payments::secureWipe(request.password);
                problem = Notice{"error", error.what()};
            }
        }
        res.status = httplib::StatusCode::UnprocessableContent_422;
        sendPage(res, visitor, "Create an account", register_.render(registrationValues(req, visitor)), problem);
    }

    void doLogout(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!acceptForm(req, res, visitor)) {
            return;
        }
        if (!visitor.sessionToken.empty()) {
            app_.auth().logout(visitor.sessionToken);
        }
        res.set_header("Set-Cookie", expiredCookie(kSessionCookie));
        res.set_redirect("/login?notice=loggedout", httplib::StatusCode::SeeOther_303);
    }

    // -------------------------------------------------------------- wallet

    void renderDashboard(httplib::Response& res, const Visitor& visitor, const std::optional<Notice>& notice) {
        const models::Wallet wallet = app_.wallets().myWallet(visitor.sessionToken);
        services::HistoryQuery recent;
        recent.limit = kRecentCount;
        const services::HistoryPage page = app_.transactions().history(visitor.sessionToken, recent);

        // Fresh idempotency keys for each form shown. Resubmitting the same
        // form (e.g. refreshing after "Pay") reuses its key, so it is
        // answered with the original result instead of paying again.
        const std::string panel =
            app_.pins().hasPin(visitor.user->id())
                ? payForm_.render({{"csrf", visitor.csrf}, {"key", services::PaymentService::newIdempotencyKey()}})
                : pinForm_.render({{"csrf", visitor.csrf}});
        sendPage(res, visitor, "Dashboard",
                 dashboard_.render({{"balance", wallet.balance().toString()},
                                    {"full_name", visitor.user->fullName()},
                                    {"upi_id", visitor.user->upiId()},
                                    {"csrf", visitor.csrf},
                                    {"deposit_key", services::PaymentService::newIdempotencyKey()},
                                    {"payment_panel", panel},
                                    {"recent", transactionTable(page.entries)}}),
                 notice);
    }

    void showDashboard(const httplib::Request&, httplib::Response& res, Visitor& visitor) {
        if (requireUser(res, visitor)) {
            renderDashboard(res, visitor, std::nullopt);
        }
    }

    void doDeposit(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!requireUser(res, visitor) || !acceptForm(req, res, visitor)) {
            return;
        }
        std::optional<Notice> notice;
        try {
            const payments::Money amount = payments::Money::parse(trim(req.get_param_value("amount")));
            const models::Transaction record =
                app_.wallets().deposit(visitor.sessionToken, amount, req.get_param_value("key"));
            notice = record.succeeded()
                         ? Notice{"success", "Deposited " + record.amount().toString() + ". Transaction ID " +
                                                 record.reference()}
                         : Notice{"error", "Deposit failed: " + record.failureReason() + ". Transaction ID " +
                                               record.reference()};
        } catch (const SessionError&) {
            throw;
        } catch (const PaySphereError& error) {
            notice = Notice{"error", error.what()};
        }
        renderDashboard(res, visitor, notice);
    }

    void doPay(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!requireUser(res, visitor) || !acceptForm(req, res, visitor)) {
            return;
        }
        std::optional<Notice> notice;
        try {
            const payments::SecretString pin(req.get_param_value("pin"));
            const payments::Money amount = payments::Money::parse(trim(req.get_param_value("amount")));
            const payments::UPIPayment payment(req.get_param_value("payee"), amount, pin.value());
            const services::PaymentResult result =
                app_.payments().pay(visitor.sessionToken, payment, req.get_param_value("key"));
            const models::Transaction& record = result.transaction;
            const std::string prefix =
                result.replayed ? "This payment was already submitted; showing its original result. " : "";
            notice = record.succeeded()
                         ? Notice{"success", prefix + "Paid " + record.amount().toString() + " to " +
                                                 payment.payeeAddress() + ". Transaction ID " + record.reference()}
                         : Notice{"error", prefix + "Payment failed: " + record.failureReason() +
                                               ". Transaction ID " + record.reference()};
        } catch (const SessionError&) {
            throw;
        } catch (const PaySphereError& error) {
            notice = Notice{"error", error.what()};
        }
        renderDashboard(res, visitor, notice);
    }

    void doSetPin(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!requireUser(res, visitor) || !acceptForm(req, res, visitor)) {
            return;
        }
        const payments::SecretString pin(req.get_param_value("pin"));
        const payments::SecretString confirm(req.get_param_value("confirm"));
        std::optional<Notice> notice;
        if (pin.value() != confirm.value()) {
            notice = Notice{"error", "The two PINs do not match. Your PIN was not set."};
        } else {
            try {
                app_.pins().setPin(visitor.sessionToken, pin.value());
                notice = Notice{"success", "UPI PIN set. You can now make payments."};
            } catch (const SessionError&) {
                throw;
            } catch (const PaySphereError& error) {
                notice = Notice{"error", std::string(error.what()) + ". Your PIN was not set."};
            }
        }
        renderDashboard(res, visitor, notice);
    }

    // ------------------------------------------------------------- history

    void showHistory(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!requireUser(res, visitor)) {
            return;
        }
        std::string filter = req.get_param_value("filter");
        if (filter != "sent" && filter != "received") {
            filter = "all";
        }
        const std::int64_t page = pageNumber(req.get_param_value("page"));

        services::HistoryQuery query;
        if (filter == "sent") {
            query.direction = models::TransactionDirection::Sent;
        } else if (filter == "received") {
            query.direction = models::TransactionDirection::Received;
        }
        query.limit = kHistoryPageSize;
        query.offset = (page - 1) * kHistoryPageSize;
        const services::HistoryPage result = app_.transactions().history(visitor.sessionToken, query);

        std::string filters;
        for (const char* option : {"all", "sent", "received"}) {
            filters += std::string("<a class=\"chip") + (filter == option ? " active" : "") +
                       "\" href=\"/history?filter=" + option + "\">" + option + "</a>";
        }
        const std::int64_t pages = (result.totalMatching + kHistoryPageSize - 1) / kHistoryPageSize;
        std::string pager;
        if (page > 1) {
            pager += "<a class=\"chip\" href=\"/history?filter=" + filter + "&amp;page=" +
                     std::to_string(page - 1) + "\">&larr; Newer</a>";
        }
        if (pages > 1) {
            pager += "<span class=\"muted\">Page " + std::to_string(page) + " of " + std::to_string(pages) + "</span>";
        }
        if (page < pages) {
            pager += "<a class=\"chip\" href=\"/history?filter=" + filter + "&amp;page=" +
                     std::to_string(page + 1) + "\">Older &rarr;</a>";
        }
        sendPage(res, visitor, "History",
                 history_.render({{"filters", filters}, {"table", transactionTable(result.entries)}, {"pager", pager}}),
                 std::nullopt);
    }

    void showReceipt(const httplib::Request& req, httplib::Response& res, Visitor& visitor) {
        if (!requireUser(res, visitor)) {
            return;
        }
        try {
            const services::Receipt receipt =
                app_.transactions().receipt(visitor.sessionToken, req.get_param_value("id"));
            sendPage(res, visitor, "Receipt", receipt_.render({{"receipt", receipt.toText()}}), std::nullopt);
        } catch (const AuthorizationError& error) {
            res.status = httplib::StatusCode::NotFound_404;
            sendPage(res, visitor, "Not found",
                     message_.render({{"heading", "Transaction not found"},
                                      {"text", error.what()},
                                      {"link", "/history"},
                                      {"link_text", "Back to history"}}),
                     std::nullopt);
        }
    }

    services::ApplicationContext& app_;
    std::mutex mutex_;
    Template layout_;
    Template login_;
    Template register_;
    Template dashboard_;
    Template payForm_;
    Template pinForm_;
    Template history_;
    Template receipt_;
    Template message_;
};

WebApp::WebApp(services::ApplicationContext& app, const std::string& webRoot)
    : impl_(std::make_unique<Impl>(app, webRoot)) {}

WebApp::~WebApp() = default;

int WebApp::bind(int port) {
    // 127.0.0.1 only: the site is reachable from this computer, not the network.
    if (port == 0) {
        const int bound = impl_->server.bind_to_any_port("127.0.0.1");
        if (bound <= 0) {
            throw PaySphereError("Cannot open a port for the web server");
        }
        return bound;
    }
    if (!impl_->server.bind_to_port("127.0.0.1", port)) {
        throw PaySphereError("Cannot listen on 127.0.0.1:" + std::to_string(port) +
                             " (is another program using that port?)");
    }
    return port;
}

void WebApp::serve() {
    impl_->server.listen_after_bind();
}

void WebApp::waitUntilReady() const {
    impl_->server.wait_until_ready();
}

void WebApp::stop() {
    impl_->server.stop();
}

} // namespace paysphere::web
