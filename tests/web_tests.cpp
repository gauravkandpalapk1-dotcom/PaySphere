// Web front end: real HTTP requests against the running server, made with
// httplib's client acting as a browser (it keeps cookies like one).

#include "TestFramework.h"
#include "TestSupport.h"

#include "models/Errors.h"
#include "services/ApplicationContext.h"
#include "web/Html.h"
#include "web/Template.h"
#include "web/WebApp.h"

#include <httplib.h>

#include <map>
#include <memory>
#include <string>
#include <thread>

using paysphere::PaySphereError;
using paysphere::services::ApplicationContext;
using paysphere::services::ApplicationOptions;
using paysphere::services::PaymentService;
using paysphere::services::SodiumPasswordHasher;
using paysphere::web::escapeHtml;
using paysphere::web::Template;
using paysphere::web::WebApp;
using testsupport::ManualClock;
using testsupport::TempDirectory;

namespace {

constexpr const char* kPassword = "correct horse battery";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return text;
}

// The application plus its web server on a free port of 127.0.0.1.
struct Site {
    TempDirectory directory;
    ManualClock clock;
    std::unique_ptr<ApplicationContext> app;
    std::unique_ptr<WebApp> web;
    std::thread server;
    int port = 0;

    Site() {
        app = std::make_unique<ApplicationContext>(
            directory.file("web.db"), ApplicationOptions{SodiumPasswordHasher::Cost::minimumForTests(), &clock});
        web = std::make_unique<WebApp>(*app, PAYSPHERE_WEB_ROOT);
        port = web->bind(0);
        server = std::thread([this] { web->serve(); });
        web->waitUntilReady();
    }
    ~Site() {
        web->stop();
        server.join();
    }
    Site(const Site&) = delete;
    Site& operator=(const Site&) = delete;

    std::int64_t totalMoney() {
        return app->database().queryInt64("SELECT coalesce(sum(balance_paise), 0) FROM wallets").value();
    }
};

// A minimal browser: remembers cookies and sends them back.
struct Browser {
    httplib::Client client;
    std::map<std::string, std::string> cookies;

    explicit Browser(const Site& site) : client("127.0.0.1", site.port) { client.set_follow_location(false); }

    httplib::Headers cookieHeader() const {
        std::string header;
        for (const auto& [name, value] : cookies) {
            header += (header.empty() ? "" : "; ") + name + "=" + value;
        }
        return header.empty() ? httplib::Headers{} : httplib::Headers{{"Cookie", header}};
    }

    void remember(const httplib::Result& result) {
        for (const auto& [name, value] : result->headers) {
            if (lower(name) != "set-cookie") {
                continue;
            }
            const std::string pair = value.substr(0, value.find(';'));
            const std::string cookieName = pair.substr(0, pair.find('='));
            const std::string cookieValue = pair.substr(pair.find('=') + 1);
            if (contains(value, "Max-Age=0")) {
                cookies.erase(cookieName);
            } else {
                cookies[cookieName] = cookieValue;
            }
        }
    }

    httplib::Result get(const std::string& path) {
        httplib::Result result = client.Get(path, cookieHeader());
        remember(result);
        return result;
    }

    // Posts a form; the CSRF token from the cookie is added unless given.
    httplib::Result post(const std::string& path, httplib::Params form) {
        if (form.find("csrf") == form.end() && cookies.count("ps_csrf") != 0) {
            form.emplace("csrf", cookies.at("ps_csrf"));
        }
        httplib::Result result = client.Post(path, cookieHeader(), form);
        remember(result);
        return result;
    }

    // ----- user journeys -----
    void signUp(const std::string& name, const std::string& phone, const std::string& fullName = "") {
        get("/register");
        const auto result = post("/register", {{"full_name", fullName.empty() ? name + " Test" : fullName},
                                               {"email", name + "@example.com"},
                                               {"phone", phone},
                                               {"upi_id", name + "@paysphere"},
                                               {"password", kPassword},
                                               {"confirm", kPassword}});
        CHECK_EQ(result->status, 303);
    }
    void logIn(const std::string& name) {
        get("/login");
        const auto result = post("/login", {{"identifier", name + "@paysphere"}, {"password", kPassword}});
        CHECK_EQ(result->status, 303);
        CHECK_EQ(result->get_header_value("Location"), "/dashboard");
    }
    httplib::Result deposit(const std::string& amount) {
        return post("/deposit", {{"amount", amount}, {"key", PaymentService::newIdempotencyKey()}});
    }
    httplib::Result setPin(const std::string& pin) { return post("/pin", {{"pin", pin}, {"confirm", pin}}); }
    httplib::Result pay(const std::string& payee, const std::string& amount, const std::string& pin = "1357",
                        const std::string& key = PaymentService::newIdempotencyKey()) {
        return post("/pay", {{"payee", payee}, {"amount", amount}, {"pin", pin}, {"key", key}});
    }
};

} // namespace

// ------------------------------------------------------------ building blocks

TEST(web_templates_escape_by_default) {
    CHECK_EQ(escapeHtml("<a href=\"x\">'Tom & Jerry'</a>"),
             "&lt;a href=&quot;x&quot;&gt;&#39;Tom &amp; Jerry&#39;&lt;/a&gt;");
    const Template page("<p>{{text}}</p>{{{html}}}");
    CHECK_EQ(page.render({{"text", "<b>bold?</b>"}, {"html", "<hr>"}}), "<p>&lt;b&gt;bold?&lt;/b&gt;</p><hr>");
    CHECK_THROWS_AS(page.render({{"text", "only one"}}), PaySphereError); // missing value is an error
}

// ------------------------------------------------------------------ serving

TEST(web_serves_pages_with_security_headers) {
    Site site;
    Browser browser(site);
    const auto login = browser.get("/login");
    CHECK_EQ(login->status, 200);
    CHECK(contains(login->body, "<form method=\"post\" action=\"/login\""));
    CHECK(contains(login->get_header_value("Content-Security-Policy"), "default-src 'none'"));
    CHECK_EQ(login->get_header_value("X-Content-Type-Options"), "nosniff");
    CHECK_EQ(login->get_header_value("Cache-Control"), "no-store");
    CHECK(contains(login->get_header_value("Set-Cookie"), "HttpOnly; SameSite=Strict"));

    const auto css = browser.get("/static/style.css");
    CHECK_EQ(css->status, 200);
    CHECK(contains(css->get_header_value("Content-Type"), "text/css"));

    const auto missing = browser.get("/no-such-page");
    CHECK_EQ(missing->status, 404);
    CHECK(contains(missing->body, "Page not found"));
}

// ----------------------------------------------------------------- journeys

TEST(web_full_journey_register_login_deposit_pay_and_logout) {
    Site site;
    Browser ravi(site);
    ravi.signUp("ravi", "9123456789");

    Browser asha(site);
    asha.signUp("asha", "9876543210");
    asha.logIn("asha");
    CHECK(contains(asha.cookies.at("ps_session"), "") && asha.cookies.at("ps_session").size() == 64);

    auto page = asha.get("/dashboard");
    CHECK_EQ(page->status, 200);
    CHECK(contains(page->body, "Rs. 0.00"));
    CHECK(contains(page->body, "Set your UPI PIN")); // no PIN yet: the PIN form is shown

    CHECK(contains(asha.setPin("1357")->body, "UPI PIN set."));
    CHECK(contains(asha.deposit("1000")->body, "Deposited Rs. 1,000.00."));
    page = asha.pay("ravi@paysphere", "250.50");
    CHECK(contains(page->body, "Paid Rs. 250.50 to ravi@paysphere."));
    CHECK(contains(page->body, "Rs. 749.50")); // updated balance on the same page

    const auto history = asha.get("/history?filter=sent");
    CHECK(contains(history->body, "Paid to ravi Test (ravi@paysphere)"));
    const std::string marker = "/receipt?id=";
    const std::size_t at = history->body.find(marker);
    CHECK(at != std::string::npos);
    const std::string reference = history->body.substr(at + marker.size(), 23);
    const auto receipt = asha.get("/receipt?id=" + reference);
    CHECK_EQ(receipt->status, 200);
    CHECK(contains(receipt->body, "UPI transfer (sent)"));

    // Logging out ends the session on the server, not just in the browser.
    const std::string oldSession = asha.cookies.at("ps_session");
    const auto out = asha.post("/logout", {});
    CHECK_EQ(out->get_header_value("Location"), "/login?notice=loggedout");
    CHECK(asha.cookies.count("ps_session") == 0);
    asha.cookies["ps_session"] = oldSession; // replaying the old cookie must not work
    CHECK_EQ(asha.get("/dashboard")->get_header_value("Location"), "/login?notice=expired");
}

TEST(web_private_pages_require_login) {
    Site site;
    Browser visitor(site);
    visitor.get("/login");
    for (const char* path : {"/dashboard", "/history", "/receipt?id=TRF1"}) {
        const auto result = visitor.get(path);
        CHECK_EQ(result->status, 303);
        CHECK_EQ(result->get_header_value("Location"), "/login?notice=signin");
    }
    CHECK_EQ(visitor.deposit("100")->status, 303); // POSTs too
    visitor.cookies["ps_session"] = std::string(64, 'a'); // a forged session
    CHECK_EQ(visitor.get("/dashboard")->get_header_value("Location"), "/login?notice=expired");
    CHECK_EQ(site.totalMoney(), 0);
}

// ----------------------------------------------------------------- security

TEST(web_rejects_forms_without_the_matching_csrf_token) {
    Site site;
    Browser asha(site);
    asha.signUp("asha", "9876543210");
    asha.logIn("asha");

    const std::string key = PaymentService::newIdempotencyKey();
    CHECK_EQ(asha.post("/deposit", {{"amount", "100"}, {"key", key}, {"csrf", std::string(32, '0')}})->status, 403);
    CHECK_EQ(asha.post("/deposit", {{"amount", "100"}, {"key", key}, {"csrf", ""}})->status, 403);
    CHECK_EQ(asha.post("/deposit?amount=100", {{"key", key}})->status, 403); // fields must not come from the URL
    CHECK_EQ(site.totalMoney(), 0);

    Browser attacker(site); // a different browser cannot reuse asha's form token
    attacker.cookies["ps_session"] = asha.cookies.at("ps_session");
    CHECK_EQ(attacker.post("/deposit", {{"amount", "100"}, {"key", key}, {"csrf", asha.cookies.at("ps_csrf")}})
                 ->status,
             403);
    CHECK_EQ(site.totalMoney(), 0);
    CHECK(contains(asha.deposit("100")->body, "Deposited")); // the genuine form still works
}

TEST(web_escapes_everything_users_typed) {
    Site site;
    Browser mallory(site);
    mallory.signUp("mallory", "9000000003", "<script>alert(1)</script>");
    mallory.logIn("mallory");
    const auto page = mallory.get("/dashboard");
    CHECK(contains(page->body, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(!contains(page->body, "<script>"));

    Browser other(site);
    other.get("/login");
    const auto failed = other.post("/login", {{"identifier", "\"><img src=x onerror=alert(1)>"}, {"password", "x"}});
    CHECK_EQ(failed->status, 401);
    CHECK(contains(failed->body, "value=\"&quot;&gt;&lt;img src=x onerror=alert(1)&gt;\""));
    CHECK(!contains(failed->body, "<img src=x"));
}

TEST(web_resubmitting_a_payment_does_not_pay_twice) {
    Site site;
    Browser ravi(site);
    ravi.signUp("ravi", "9123456789");
    Browser asha(site);
    asha.signUp("asha", "9876543210");
    asha.logIn("asha");
    asha.setPin("1357");
    asha.deposit("1000");

    // The browser re-sends the same form, e.g. when the user refreshes after "Pay".
    const std::string key = PaymentService::newIdempotencyKey();
    CHECK(contains(asha.pay("ravi@paysphere", "300", "1357", key)->body, "Paid Rs. 300.00"));
    const auto again = asha.pay("ravi@paysphere", "300", "1357", key);
    CHECK(contains(again->body, "already submitted"));
    CHECK(contains(again->body, "Rs. 700.00")); // debited once
    CHECK_EQ(site.totalMoney(), 100000);
}

TEST(web_shows_errors_and_failed_payments_clearly) {
    Site site;
    Browser asha(site);
    asha.signUp("asha", "9876543210");

    asha.get("/login");
    const auto wrong = asha.post("/login", {{"identifier", "asha@paysphere"}, {"password", "wrong password 1"}});
    CHECK_EQ(wrong->status, 401);
    CHECK(contains(wrong->body, "Incorrect email/UPI ID or password"));

    asha.logIn("asha");
    CHECK(contains(asha.setPin("1234")->body, "Your PIN was not set")); // too easy
    CHECK(contains(asha.post("/pin", {{"pin", "1357"}, {"confirm", "2468"}})->body, "do not match"));
    asha.setPin("1357");
    CHECK(contains(asha.deposit("abc")->body, "Invalid amount"));
    asha.deposit("100");
    CHECK(contains(asha.pay("nobody@paysphere", "10")->body, "Payment failed: Recipient UPI ID not found"));
    Browser ravi(site);
    ravi.signUp("ravi", "9123456789");
    CHECK(contains(asha.pay("ravi@paysphere", "500")->body, "Payment failed: Insufficient balance"));

    const auto missing = asha.get("/receipt?id=TRF00000000NOTREAL0000");
    CHECK_EQ(missing->status, 404);
    CHECK(contains(missing->body, "Transaction not found"));

    const auto duplicate = asha.post("/register", {{"full_name", "Asha Again"},
                                                   {"email", "asha@example.com"},
                                                   {"phone", "9000000009"},
                                                   {"upi_id", "asha2@paysphere"},
                                                   {"password", kPassword},
                                                   {"confirm", kPassword}});
    CHECK_EQ(duplicate->status, 422);
    CHECK(contains(duplicate->body, "already exists"));
    CHECK(contains(duplicate->body, "value=\"asha2@paysphere\"")); // keeps what was typed (not passwords)
    CHECK(!contains(duplicate->body, kPassword));
}

TEST(web_receipts_are_private_to_the_parties) {
    Site site;
    Browser ravi(site);
    ravi.signUp("ravi", "9123456789");
    Browser meera(site);
    meera.signUp("meera", "9000000003");
    Browser asha(site);
    asha.signUp("asha", "9876543210");
    asha.logIn("asha");
    asha.setPin("1357");
    asha.deposit("100");
    asha.pay("ravi@paysphere", "40");

    const auto history = asha.get("/history");
    const std::string marker = "/receipt?id=TRF";
    const std::string reference = history->body.substr(history->body.find(marker) + 12, 23);

    ravi.logIn("ravi");
    CHECK(contains(ravi.get("/receipt?id=" + reference)->body, "UPI transfer (received)"));
    meera.logIn("meera");
    CHECK_EQ(meera.get("/receipt?id=" + reference)->status, 404);
}
