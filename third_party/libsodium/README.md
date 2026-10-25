# libsodium

* Version: 1.0.22. File: `libsodium-1.0.22-mingw.tar.gz` from
  https://download.libsodium.org/libsodium/releases/.
* Integrity: the archive's minisign signature was verified against
  libsodium's published key
  `RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3`.
  The key comes from https://doc.libsodium.org/installation.
  The archive's SHA-256 is
  `1d99e0afaf27bce664249232e9dc628ae6bb7b49f0ab53ad6db372b028a35d9d`.
* License: ISC (https://github.com/jedisct1/libsodium/blob/master/LICENSE).
* Files: `include/` and `lib/libsodium.a`, taken from the archive's
  `libsodium-win64` directory. They are unmodified.

The top-level `CMakeLists.txt` links it statically, with `SODIUM_STATIC`, on
64-bit MinGW. Other platforms use the system package found through pkg-config,
for example `apt install libsodium-dev`.

PaySphere uses:
* `crypto_pwhash_str*` for Argon2id password hashing;
* `randombytes_buf` for session tokens;
* `crypto_generichash` (BLAKE2b) to hash session tokens before storing them;
* `sodium_memzero`.
