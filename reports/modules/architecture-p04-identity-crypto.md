# P04 production crypto adapter design (in progress)

Scope: infrastructure/identity_crypto/{include,src,tests}, minimal doc/05 and doc/08 notes,
and this report. Core, PostgreSQL, build configuration, protocols/API and protected doc/18
remain unchanged. This is a factory proposal, not an implemented production capability.

Factories consume trusted move-only symmetric keys, require exactly 32 bytes and reject duplicate
or empty references. Secure storage uses sodium_malloc/sodium_free. Separate immutable keyrings
are supplied for token HMAC and receipt AEAD. No request-provided URLs or JSON key sources.
Password policy is libsodium Argon2id13, fixed opslimit 2 / memlimit 64 MiB; verification first
bounds PHC length and validates exact supported parameter/version tuples before library calls.
HMAC-SHA256 uses separate canonical domain tags for token purpose and request commitments.
Canonical fields use explicit variant/version tags, fixed-width integers and length prefixes.
Receipt AEAD uses XChaCha20-Poly1305 with every binding field authenticated. Typed payload codec
has a version, variant and bounded lengths, and rejects trailing bytes and variant mismatch.

Interface observation: SecretReceiptCryptoPort::open has no now argument. The proposed factory
authenticates timestamps and bounds the retention window, without claiming current expiry
validation. The application uses current database time before open; no local wall clock is used.
Caller must still recheck current credential/session/rotation for every replay. Expensive crypto
runs outside transactions. Delegation and peer factories follow after this first capability.

Documentation impact: doc/05 will describe implemented adapter behavior and error distinctions;
doc/08 will describe trusted key provisioning and application-owned database-time expiry checks. No wire/API or
Core responsibility changes are proposed, so proto/, api/, doc/01 and doc/02 need no changes.
Protected doc/18 is read-only. No implementation, build, security or runtime acceptance is yet claimed.

## Password and random implementation checks

Implemented secure allocation/copy, bounded random bytes and fixed Argon2id password port.
Actual checks: Apple clang++ C++20 with -Wall -Wextra -Wpedantic -Werror and
-fsanitize=address,undefined linked the fixed architecture-installed libsodium.a; executed
build/identity-crypto/password_crypto_test successfully. Tests cover password success/rejection,
unsupported PHC cost rejection, random upper bound and secure-copy size. Format16 dry-run and
git diff --check pass. Tidy22 clang-analyzer/bugprone checks require explicit macOS SDK and libc++
include paths; initial default invocation had no enabled checks, then missing libc++ headers;
corrected invocation found a widening multiplication and it was fixed. No root CMake or full
service checks: build wiring belongs to the build task and no service is assembled here.
Updated docs: doc/05-infrastructure.md, doc/08-operations.md. No protocol/API changes or protected
baseline edits. Token and receipt factories are still proposals pending implementation acceptance.

## Token implementation checks

Token HMAC and typed commitment implemented, with no raw secret hash or secret serialization
buffer. HMAC state is erased on destruction. Real libsodium ASan/UBSan -Werror test passed:
correct token, cross-purpose rejection, unknown key, changed request rotation/secret, mismatched
scope/parameter variant, empty keyring. Same compiler/include/link command as password test with
src/token_crypto.cpp and tests/token_crypto_test.cpp added. Tidy22 clang-analyzer/bugprone passed
with the explicit SDK/libc++ include options described above. No JWT/AEAD acceptance yet.
