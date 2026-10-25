# SQLite amalgamation

* Version: 3.54.0 (`sqlite-amalgamation-3540000.zip` from https://sqlite.org/download.html).
  The archive's SHA3-256 was checked against the value published there.
* License: public domain (https://sqlite.org/copyright.html).
* Files: `sqlite3.c`, `sqlite3.h`, unmodified.

It is compiled as a static library by the top-level `CMakeLists.txt`. Set
`-DPAYSPHERE_USE_SYSTEM_SQLITE=ON` to use an installed SQLite instead.
