#pragma once

// Private to the services layer: libsodium initialization and small helpers
// shared by several services.

#include <cstddef>
#include <string>

namespace paysphere::services::detail {

// Calls sodium_init() once per process (thread-safe). Throws PaySphereError
// if libsodium cannot initialize, e.g. when no secure random source exists.
void ensureSodiumInitialized();

// Lowercase hexadecimal encoding.
std::string toHex(const unsigned char* bytes, std::size_t length);

// BLAKE2b-256 of the text, as 64 hex characters.
std::string blake2bHex(const std::string& text);

// `byteCount` bytes from the operating system's CSPRNG, hex encoded.
std::string randomHex(std::size_t byteCount);

} // namespace paysphere::services::detail
