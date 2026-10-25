// PaySphere command-line front end, for demonstrating the backend.
//
// Everything here is input and output: reading what the user types, calling
// a service through its public interface and printing the result. All rules
// (validation, authorization, limits, atomic money movement, auditing) live in
// the services built by services::ApplicationContext.

#include "database/Database.h"
#include "database/DatabaseManager.h"
#include "models/Errors.h"
#include "models/Transaction.h"
#include "models/User.h"
#include "payments/Money.h"
#include "payments/SecretString.h"
#include "payments/UPIPayment.h"
#include "services/ApplicationContext.h"
#include "web/WebApp.h"

#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX // keep <windows.h> from defining min/max macros
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#ifndef PAYSPHERE_VERSION
#define PAYSPHERE_VERSION "0.0.0"
#endif
#ifndef PAYSPHERE_DEFAULT_DB_PATH
#define PAYSPHERE_DEFAULT_DB_PATH "data/paysphere.db"
#endif
#ifndef PAYSPHERE_WEB_ROOT
#define PAYSPHERE_WEB_ROOT "web"
#endif

namespace {

using namespace paysphere;

constexpr int kDefaultWebPort = 8080;

// Thrown when input ends (Ctrl+Z / Ctrl+D, or the end of a piped script).
struct EndOfInput {};

// Wipes a string that held a secret when the scope ends, however it ends.
struct WipeOnExit {
    std::string& text;
    ~WipeOnExit() { payments::secureWipe(text); }
};

// Hides typed characters while a password or PIN is entered on a terminal.
// Does nothing when input is redirected from a file or pipe.
class EchoGuard {
public:
    explicit EchoGuard(std::ostream& out) : out_(out) {
#ifdef _WIN32
        handle_ = GetStdHandle(STD_INPUT_HANDLE);
        if (handle_ != INVALID_HANDLE_VALUE && GetConsoleMode(handle_, &savedMode_) != 0) {
            active_ = SetConsoleMode(handle_, savedMode_ & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
        }
#else
        if (isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &saved_) == 0) {
            termios silent = saved_;
            silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &silent) == 0;
        }
#endif
    }
    ~EchoGuard() {
        if (active_) {
            out_ << '\n'; // the Enter key was not echoed either
#ifdef _WIN32
            SetConsoleMode(handle_, savedMode_);
#else
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
#endif
        }
    }
    EchoGuard(const EchoGuard&) = delete;
    EchoGuard& operator=(const EchoGuard&) = delete;

    bool active() const noexcept { return active_; }

private:
    std::ostream& out_;
    bool active_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD savedMode_ = 0;
#else
    termios saved_{};
#endif
};

class Cli {
public:
    Cli(services::ApplicationContext& app, std::istream& in, std::ostream& out, bool terminalInput)
        : app_(app), in_(in), out_(out), terminalInput_(terminalInput) {}

    void run() {
        try {
            for (bool running = true; running;) {
                running = loggedIn() ? userMenu() : guestMenu();
            }
        } catch (const EndOfInput&) {
            out_ << '\n';
        }
        if (loggedIn()) {
            app_.auth().logout(token_);
        }
        out_ << "Goodbye.\n";
    }

private:
    bool loggedIn() const noexcept { return !token_.empty(); }

    std::string readLine(const std::string& prompt, bool secret) {
        out_ << prompt << std::flush;
        std::string line;
        if (!std::getline(in_, line)) {
            throw EndOfInput{};
        }
        if (!line.empty() && line.back() == '\r') { // scripts saved with Windows line endings
            line.pop_back();
        }
        if (!secret) { // surrounding spaces are never meaningful, except in passwords
            const auto first = line.find_first_not_of(" \t");
            line = first == std::string::npos ? std::string()
                                              : line.substr(first, line.find_last_not_of(" \t") - first + 1);
        }
        if (!terminalInput_) {
            // Piped input is not echoed by a terminal; show it so a scripted
            // transcript reads like a real session, but never show secrets.
            out_ << (secret ? std::string("****") : line) << '\n';
        }
        return line;
    }

    std::string ask(const std::string& prompt) { return readLine(prompt, false); }

    // Passwords and PINs are held in SecretString, which wipes them from
    // memory as soon as they go out of scope.
    payments::SecretString askSecret(const std::string& prompt) {
        if (!terminalInput_) {
            return payments::SecretString(readLine(prompt, true));
        }
        const EchoGuard hidden(out_);
        return payments::SecretString(readLine(prompt, true));
    }

    // Runs one menu action and reports any failure without leaving the menu.
    template <typename Action>
    void attempt(Action action) {
        try {
            action();
        } catch (const SessionError& error) {
            token_.clear();
            user_.reset();
            out_ << "Session ended: " << error.what() << '\n';
        } catch (const PaySphereError& error) {
            out_ << "Error: " << error.what() << '\n';
        } catch (const std::exception& error) {
            out_ << "Unexpected error: " << error.what() << '\n';
        }
    }

    bool guestMenu() {
        out_ << "\n--- Menu ---\n1. Register\n2. Login\n0. Exit\n";
        const std::string choice = ask("Choose: ");
        if (choice == "1") {
            attempt([this] { registerUser(); });
        } else if (choice == "2") {
            attempt([this] { login(); });
        } else if (choice == "0") {
            return false;
        } else {
            out_ << "Please choose one of the listed options.\n";
        }
        return true;
    }

    bool userMenu() {
        out_ << "\n--- " << user_->fullName() << " (" << user_->upiId() << ") ---\n"
             << "1. Check balance\n2. Deposit money\n3. Pay by UPI ID\n4. Transaction history\n"
             << "5. Find transaction / show receipt\n6. Set UPI PIN\n7. Logout\n0. Exit\n";
        const std::string choice = ask("Choose: ");
        if (choice == "1") {
            attempt([this] { showBalance(); });
        } else if (choice == "2") {
            attempt([this] { deposit(); });
        } else if (choice == "3") {
            attempt([this] { pay(); });
        } else if (choice == "4") {
            attempt([this] { showHistory(); });
        } else if (choice == "5") {
            attempt([this] { showReceipt(); });
        } else if (choice == "6") {
            attempt([this] { setPin(); });
        } else if (choice == "7") {
            attempt([this] { logout(); });
        } else if (choice == "0") {
            return false;
        } else {
            out_ << "Please choose one of the listed options.\n";
        }
        return true;
    }

    void registerUser() {
        services::RegistrationRequest request;
        request.fullName = ask("Full name: ");
        request.email = ask("Email: ");
        request.phone = ask("Mobile number: ");
        request.upiId = ask("Choose a UPI ID (e.g. name@paysphere): ");
        const payments::SecretString password = askSecret("Password (8-128 characters): ");
        if (askSecret("Confirm password: ").value() != password.value()) {
            out_ << "Error: the passwords do not match.\n";
            return;
        }
        request.password = password.value();
        const WipeOnExit wipe{request.password};
        const models::User user = app_.auth().registerUser(request);
        out_ << "Registered " << user.fullName() << " (" << user.upiId()
             << "). A wallet with Rs. 0.00 was created. Log in to continue.\n";
    }

    void login() {
        const std::string identifier = ask("Email or UPI ID: ");
        const payments::SecretString password = askSecret("Password: ");
        services::LoginResult result = app_.auth().login(identifier, password.value());
        token_ = result.sessionToken;
        user_ = result.user;
        out_ << "Welcome, " << user_->fullName() << ".\n";
        if (!app_.pins().hasPin(user_->id())) {
            out_ << "Tip: set a UPI PIN (option 6) before making payments.\n";
        }
    }

    void showBalance() {
        out_ << "Balance: " << app_.wallets().balance(token_) << '\n';
    }

    void deposit() {
        const payments::Money amount = payments::Money::parse(ask("Amount in rupees: "));
        const models::Transaction record = app_.wallets().deposit(token_, amount);
        if (record.succeeded()) {
            out_ << "Deposited " << record.amount() << ". Transaction ID " << record.reference() << '\n';
        } else {
            out_ << "Deposit FAILED: " << record.failureReason() << ". Transaction ID " << record.reference()
                 << '\n';
        }
    }

    void pay() {
        const std::string payee = ask("Payee UPI ID: ");
        const payments::Money amount = payments::Money::parse(ask("Amount in rupees: "));
        const payments::SecretString pin = askSecret("UPI PIN: ");
        const payments::UPIPayment payment(payee, amount, pin.value()); // keeps its own self-wiping copy
        // One key per attempt. A real client would keep it and resend the same
        // key if the request timed out, so the payment could not happen twice.
        const services::PaymentResult result =
            app_.payments().pay(token_, payment, services::PaymentService::newIdempotencyKey());
        const models::Transaction& record = result.transaction;
        if (record.succeeded()) {
            out_ << "Paid " << record.amount() << " to " << payment.payeeAddress() << ". Transaction ID "
                 << record.reference() << '\n';
        } else {
            out_ << "Payment FAILED: " << record.failureReason() << ". Transaction ID " << record.reference()
                 << '\n';
        }
    }

    void showHistory() {
        services::HistoryQuery query;
        const std::string filter = ask("Show 1. All  2. Sent  3. Received: ");
        if (filter == "2") {
            query.direction = models::TransactionDirection::Sent;
        } else if (filter == "3") {
            query.direction = models::TransactionDirection::Received;
        }
        const services::HistoryPage page = app_.transactions().history(token_, query);
        if (page.entries.empty()) {
            out_ << "No transactions to show.\n";
            return;
        }
        out_ << "Showing " << page.entries.size() << " of " << page.totalMatching
             << " transaction(s), newest first:\n"
             << std::left << std::setw(30) << "DATE (IST)" << std::setw(10) << "TYPE" << std::setw(24)
             << "COUNTERPARTY" << std::setw(18) << "AMOUNT" << std::setw(9) << "STATUS" << "ID\n";
        for (const services::HistoryEntry& entry : page.entries) {
            const models::Transaction& t = entry.transaction;
            const bool deposit = t.type() == models::TransactionType::Deposit;
            const char* type = deposit ? "Deposit"
                               : entry.direction == models::TransactionDirection::Sent ? "Sent"
                                                                                       : "Received";
            const std::string counterparty =
                entry.counterparty ? entry.counterparty->upiId : (deposit ? "(bank, simulated)" : "(unknown)");
            out_ << std::setw(30) << services::formatIndianTime(services::parseTimestamp(t.createdAt()))
                 << std::setw(10) << type << std::setw(24) << counterparty << std::setw(18)
                 << t.amount().toString() << std::setw(9) << models::toString(t.status()) << t.reference()
                 << '\n';
        }
    }

    void showReceipt() {
        out_ << app_.transactions().receipt(token_, ask("Transaction ID: ")).toText();
    }

    void setPin() {
        const payments::SecretString pin = askSecret("New UPI PIN (4 or 6 digits): ");
        if (askSecret("Confirm UPI PIN: ").value() != pin.value()) {
            out_ << "Error: the PINs do not match.\n";
            return;
        }
        app_.pins().setPin(token_, pin.value());
        out_ << "UPI PIN set.\n";
    }

    void logout() {
        app_.auth().logout(token_);
        token_.clear();
        user_.reset();
        out_ << "Logged out.\n";
    }

    services::ApplicationContext& app_;
    std::istream& in_;
    std::ostream& out_;
    bool terminalInput_;
    std::string token_; // the session token; never printed
    std::optional<models::User> user_;
};

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [--db <path>] [--web [port]] [--version] [--help]\n"
              << "  --db <path>   SQLite database file (default: " PAYSPHERE_DEFAULT_DB_PATH ")\n"
              << "  --web [port]  Serve the web front end on http://127.0.0.1:<port>/ (default port "
              << kDefaultWebPort << ")\n"
              << "  --version     Print the version and exit\n"
              << "  --help        Print this message and exit\n"
              << "Without --web, starts the interactive menu. Input can also be piped from a script.\n";
}

bool isPortNumber(const std::string& text) {
    return !text.empty() && text.size() <= 5 && text.find_first_not_of("0123456789") == std::string::npos &&
           std::stoi(text) >= 1 && std::stoi(text) <= 65535;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string databasePath = PAYSPHERE_DEFAULT_DB_PATH;
    bool webMode = false;
    int webPort = kDefaultWebPort;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (argument == "--version") {
            std::cout << "PaySphere " PAYSPHERE_VERSION "\n";
            return 0;
        }
        if (argument == "--db" && i + 1 < argc) {
            databasePath = argv[++i];
            continue;
        }
        if (argument == "--web") {
            webMode = true;
            if (i + 1 < argc && isPortNumber(argv[i + 1])) {
                webPort = std::stoi(argv[++i]);
            }
            continue;
        }
        std::cerr << "Unknown or incomplete argument: " << argument << "\n\n";
        printUsage(argv[0]);
        return 2;
    }

    try {
        services::ApplicationContext app(databasePath);
        std::cout << "PaySphere " PAYSPHERE_VERSION " - Digital Wallet & UPI Payment Simulator\n"
                  << "Simulated money only: no real bank or UPI network is contacted.\n"
                  << "Database: " << app.database().path() << " (schema v" << app.database().schemaVersion()
                  << ", SQLite " << db::Database::libraryVersion() << ")\n";

        if (webMode) {
            web::WebApp site(app, PAYSPHERE_WEB_ROOT);
            const int port = site.bind(webPort);
            std::cout << "Web front end running: open http://127.0.0.1:" << port << "/ in your browser.\n"
                      << "Only this computer can connect. Press Ctrl+C to stop.\n"
                      << std::flush;
            site.serve();
            return 0;
        }

#ifdef _WIN32
        const bool terminalInput = GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
        const bool terminalInput = isatty(STDIN_FILENO) != 0;
#endif
        Cli(app, std::cin, std::cout, terminalInput).run();
        return 0;
    } catch (const PaySphereError& error) {
        std::cerr << "Error [" << error.code() << "]: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
    }
    return 1;
}
