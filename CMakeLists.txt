cmake_minimum_required(VERSION 3.16)

project(PaySphere
    VERSION 0.9.0
    DESCRIPTION "Digital Wallet & UPI Payment Simulator"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PAYSPHERE_BUILD_TESTS "Build the unit tests" ON)
option(PAYSPHERE_USE_SYSTEM_SQLITE "Link an installed SQLite3 instead of the bundled amalgamation" OFF)
option(PAYSPHERE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# --------------------------------------------------------------------------
# SQLite
# --------------------------------------------------------------------------
if(PAYSPHERE_USE_SYSTEM_SQLITE)
    # STRICT tables, used by the schema, need SQLite 3.37 or newer.
    find_package(SQLite3 3.37 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(PAYSPHERE_SQLITE_TARGET SQLite3::SQLite3)
    else()
        set(PAYSPHERE_SQLITE_TARGET SQLite::SQLite3)
    endif()
else()
    add_library(paysphere_sqlite STATIC third_party/sqlite/sqlite3.c)
    target_include_directories(paysphere_sqlite SYSTEM PUBLIC third_party/sqlite)
    target_compile_definitions(paysphere_sqlite PRIVATE
        SQLITE_DQS=0                  # reject "string" used as an identifier
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_LOAD_EXTENSION)   # no runtime loading of native code
    if(MSVC)
        target_compile_options(paysphere_sqlite PRIVATE /w)
    else()
        target_compile_options(paysphere_sqlite PRIVATE -w)
    endif()
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(paysphere_sqlite PUBLIC Threads::Threads m)
    endif()
    set(PAYSPHERE_SQLITE_TARGET paysphere_sqlite)
endif()

# --------------------------------------------------------------------------
# libsodium (Argon2id password hashing, CSPRNG, BLAKE2b)
# --------------------------------------------------------------------------
set(PAYSPHERE_BUNDLED_SODIUM_DIR "${PROJECT_SOURCE_DIR}/third_party/libsodium")
if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS "${PAYSPHERE_BUNDLED_SODIUM_DIR}/lib/libsodium.a")
    set(PAYSPHERE_SODIUM_DEFAULT ON)
else()
    set(PAYSPHERE_SODIUM_DEFAULT OFF)
endif()
option(PAYSPHERE_USE_BUNDLED_SODIUM "Use the bundled 64-bit MinGW libsodium" ${PAYSPHERE_SODIUM_DEFAULT})

if(PAYSPHERE_USE_BUNDLED_SODIUM)
    add_library(paysphere_sodium STATIC IMPORTED)
    set_target_properties(paysphere_sodium PROPERTIES
        IMPORTED_LOCATION "${PAYSPHERE_BUNDLED_SODIUM_DIR}/lib/libsodium.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PAYSPHERE_BUNDLED_SODIUM_DIR}/include"
        INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
        INTERFACE_LINK_LIBRARIES advapi32)   # RtlGenRandom, the Windows CSPRNG
    set(PAYSPHERE_SODIUM_TARGET paysphere_sodium)
else()
    # Linux/macOS: install libsodium-dev (apt) or libsodium (brew).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
    set(PAYSPHERE_SODIUM_TARGET PkgConfig::SODIUM)
endif()

# --------------------------------------------------------------------------
# Shared compiler settings
# --------------------------------------------------------------------------
function(paysphere_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(PAYSPHERE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Woverloaded-virtual)
        if(PAYSPHERE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# MinGW executables otherwise depend on libstdc++/libgcc DLLs being on PATH,
# and an older MinGW on PATH can supply incompatible copies. Link statically.
function(paysphere_configure_executable target)
    paysphere_configure_target(${target})
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
    endif()
endfunction()

# --------------------------------------------------------------------------
# Core library: models, payments, persistence and services
# --------------------------------------------------------------------------
add_library(paysphere_core STATIC
    src/models/Errors.cpp
    src/models/User.cpp
    src/models/Wallet.cpp
    src/models/Transaction.cpp
    src/models/Session.cpp
    src/payments/Money.cpp
    src/payments/UpiId.cpp
    src/payments/PaymentMethod.cpp
    src/payments/UPIPayment.cpp
    src/payments/SecretString.cpp
    src/database/DatabaseError.cpp
    src/database/SqliteError.cpp
    src/database/Database.cpp
    src/database/Statement.cpp
    src/database/DbTransaction.cpp
    src/database/SchemaMigrator.cpp
    src/database/DatabaseManager.cpp
    src/database/UserRepository.cpp
    src/database/WalletRepository.cpp
    src/database/LoginAttemptRepository.cpp
    src/database/TransactionRepository.cpp
    src/database/UpiPinRepository.cpp
    src/database/IdempotencyRepository.cpp
    src/services/Sodium.cpp
    src/services/Clock.cpp
    src/services/InputValidator.cpp
    src/services/PasswordHasher.cpp
    src/services/SessionManager.cpp
    src/services/AuthService.cpp
    src/services/ReferenceGenerator.cpp
    src/services/IdempotencyLedger.cpp
    src/services/WalletService.cpp
    src/services/UpiPinService.cpp
    src/services/RiskEngine.cpp
    src/services/PaymentService.cpp
    src/services/Receipt.cpp
    src/services/TransactionService.cpp
    src/services/ApplicationContext.cpp)
target_include_directories(paysphere_core PUBLIC include)
# PRIVATE: public headers include neither sqlite3.h nor sodium.h, so callers
# stay independent of the storage engine and the crypto library.
target_link_libraries(paysphere_core PRIVATE ${PAYSPHERE_SQLITE_TARGET} ${PAYSPHERE_SODIUM_TARGET})
paysphere_configure_target(paysphere_core)

# --------------------------------------------------------------------------
# Web front end (HTML/CSS pages served on 127.0.0.1 by cpp-httplib)
# --------------------------------------------------------------------------
add_library(paysphere_web STATIC
    src/web/Html.cpp
    src/web/Template.cpp
    src/web/WebApp.cpp)
target_link_libraries(paysphere_web PUBLIC paysphere_core)
# PRIVATE: WebApp.h uses the pimpl idiom, so httplib.h stays out of its users.
target_include_directories(paysphere_web SYSTEM PRIVATE third_party/httplib)
if(WIN32)
    target_link_libraries(paysphere_web PUBLIC ws2_32) # Windows sockets
    # cpp-httplib needs Windows 10 APIs; MinGW's headers default to an older target.
    target_compile_definitions(paysphere_web PUBLIC _WIN32_WINNT=0x0A00)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(paysphere_web PUBLIC Threads::Threads)
endif()
paysphere_configure_target(paysphere_web)
set(PAYSPHERE_WEB_ROOT "${PROJECT_SOURCE_DIR}/web")

# --------------------------------------------------------------------------
# Application
# --------------------------------------------------------------------------
add_executable(paysphere src/main.cpp)
target_link_libraries(paysphere PRIVATE paysphere_web)
target_compile_definitions(paysphere PRIVATE
    "PAYSPHERE_VERSION=\"${PROJECT_VERSION}\""
    "PAYSPHERE_DEFAULT_DB_PATH=\"${PROJECT_SOURCE_DIR}/data/paysphere.db\""
    "PAYSPHERE_WEB_ROOT=\"${PAYSPHERE_WEB_ROOT}\"")
paysphere_configure_executable(paysphere)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(PAYSPHERE_BUILD_TESTS)
    enable_testing()
    add_executable(paysphere_tests
        tests/test_main.cpp
        tests/test_money.cpp
        tests/test_models.cpp
        tests/test_database.cpp
        tests/test_schema.cpp
        tests/test_database_manager.cpp
        tests/test_validation.cpp
        tests/auth_tests.cpp
        tests/wallet_tests.cpp
        tests/payment_tests.cpp
        tests/transaction_tests.cpp
        tests/e2e_tests.cpp
        tests/qa_tests.cpp
        tests/web_tests.cpp)
    target_link_libraries(paysphere_tests PRIVATE paysphere_web)
    # The web tests act as a browser with httplib's client.
    target_include_directories(paysphere_tests SYSTEM PRIVATE third_party/httplib)
    target_compile_definitions(paysphere_tests PRIVATE "PAYSPHERE_WEB_ROOT=\"${PAYSPHERE_WEB_ROOT}\"")
    paysphere_configure_executable(paysphere_tests)
    add_test(NAME paysphere_tests COMMAND paysphere_tests)

    # Drives the real executable with scripted input across three runs.
    add_test(NAME cli_end_to_end
        COMMAND ${CMAKE_COMMAND}
            -DPAYSPHERE_EXE=$<TARGET_FILE:paysphere>
            -DSCRIPT_DIR=${PROJECT_SOURCE_DIR}/tests/cli
            -DWORK_DIR=${PROJECT_BINARY_DIR}/cli-e2e
            -P ${PROJECT_SOURCE_DIR}/tests/cli/cli_e2e.cmake)
endif()
