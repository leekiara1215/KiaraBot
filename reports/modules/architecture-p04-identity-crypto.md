# P04 production crypto adapter implementation (in progress)

Scope: infrastructure/identity_crypto/{include,src,tests}, minimal doc/05 and doc/08 notes,
and this report. Core, PostgreSQL, build configuration, protocols/API and protected doc/18
remain unchanged. Password, Random, Token and SecretReceipt adapters are implemented below; delegation and peer
verification remain in progress. No production service composition is claimed.

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
Protected doc/18 is read-only. The dated implementation check sections below supersede this initial proposal status; no complete
service/runtime acceptance is claimed.

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

## Receipt implementation acceptance

Implemented immutable trusted keyring and XChaCha20-Poly1305, full canonical AAD and strict typed
secure-memory codec. Tests pass using real libsodium: both payload roundtrips, binding mutations,
nonce/ciphertext/key/version tampering, maximum retention window, wrong payload variant. An
independent test AAD encoder successfully encrypts a valid credential delivery before testing
AEAD-authenticated truncated/unknown version/unknown variant/impossible length/trailing payloads.
No current expiry claim: application must check authoritative DB time and live credential/rotation.
Actual command: clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror
-fsanitize=address,undefined -Icore/include -Iinfrastructure/identity_crypto/include
-I<fixed-prefix>/include src/password_crypto.cpp src/receipt_crypto.cpp tests/receipt_crypto_test.cpp
<fixed-prefix>/lib/libsodium.a -o build/identity-crypto/receipt_crypto_test (src/tests paths are under
infrastructure/identity_crypto). The built executable passed. Fixed-prefix is the authorized shared
kiarabot-toolchain/build/architecture-installed/arm64-osx. Format16 and tidy22 analyzer/bugprone
with explicit SDK/libc++ flags passed. Root build/CTest remain for build task; no CMake edits here.
Include order follows doc/12 using localized formatter guards because the existing .clang-format
IncludeBlocks: Regroup otherwise moves the corresponding factory header behind system includes.

## Review hardening follow-up

PHC salt/hash now use sodium_base642bin and sodium_bin2base64 canonical roundtrip with exact
16/32-byte lengths. Added malformed noncanonical salt/hash last-character rejection tests;
correct password and wrong-password false tests still pass. Allocation failure on practical
password/factory/token result boundaries returns UpstreamUnavailable. Existing formatter conflicts
are locally guarded for required corresponding-header/std/third-party order. Rebuilt password
and token tests with -Werror + ASan/UBSan, executed successfully; tidy22 analyzer/bugprone and
format16 passed. Boundary and inner architecture checks pass; attempted nonexistent legacy
check-inner-purity.py was corrected to check-architecture.py --inner. No external interfaces,
Core models, API/proto, PG or protected baseline changed; doc/01/02/18 require no synchronization.

## Reproducible first-stage checks

Run from the operations worktree (paths below are read-only shared dependencies, not repository
configuration). Three test commands were run with the following common flags and inputs:

```sh
crypto_prefix=/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain/build/architecture-installed/arm64-osx
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -Icore/include -Iinfrastructure/identity_crypto/include -I"$crypto_prefix/include" \
  infrastructure/identity_crypto/src/password_crypto.cpp \
  infrastructure/identity_crypto/tests/password_crypto_test.cpp \
  "$crypto_prefix/lib/libsodium.a" -o build/identity-crypto/password_crypto_test
build/identity-crypto/password_crypto_test
```

For Token, the same command additionally compiles `src/token_crypto.cpp` and substitutes
`tests/token_crypto_test.cpp` and the corresponding executable name. For Receipt, it additionally
compiles `src/receipt_crypto.cpp` and substitutes `tests/receipt_crypto_test.cpp` and executable name.
All abbreviated source/test names in this paragraph are under `infrastructure/identity_crypto/`.

```sh
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
```

Both pass. `git diff --check` and fixed clang-format16 `--dry-run --Werror` pass. Tidy22 used
`--checks='-*,clang-analyzer-*,bugprone-*' --warnings-as-errors='*'` with the same includes,
`-std=c++20 -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
-isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1`.
Diagnostics in third-party/system headers are suppressed; no user-source diagnostics remain.
No root full compile database/CTest registration exists for these additions until build integration;
that full repository gate is deliberately not represented as passed by these manual adapter tests.

## Peer certificate adapter acceptance

Public factory: peer_certificate.hpp PeerTrustConfig consumes trusted public PEM roots and a fixed
URI/DNS SAN kind. Implementation validates leaf-first concatenated DER, bounded eight certs/64KiB,
strict OpenSSL chain at passed now, clientAuth EKU, fixed strong algorithm set, actual leaf SHA256
fingerprint, exact NUL-free SAN and authoritative expected peer/subject/platform-service/credential
shape. Caller must supply current credential registration and real transport evidence; no DB or
body certificate parsing and no independent claim of live credential/revocation status.

Actual verification command from operations worktree:

```sh
crypto_prefix=/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain/build/architecture-installed/arm64-osx
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -Icore/include -Iinfrastructure/identity_crypto/include -isystem "$crypto_prefix/include" \
  infrastructure/identity_crypto/src/peer_certificate.cpp \
  infrastructure/identity_crypto/tests/peer_certificate_test.cpp \
  "$crypto_prefix/lib/libcrypto.a" -o build/identity-crypto/peer_certificate_test
build/identity-crypto/peer_certificate_test
```

Passed with real OpenSSL3.5.0 Ed25519 CA and leaf generated in test memory. Negative paths include
wrong SAN/fingerprint/service/credential/time/root, duplicate/trailing/empty/oversized DER,
serverAuth-only EKU, unknown service enum, certificate expiry despite extended registration and
malformed PEM trailing garbage. Tidy22 analyzer/bugprone source+test passed; shared external include
is marked system to avoid OpenSSL macro diagnostics. Format16, boundary/inner and diff checks pass.
Docs synchronized: doc/05 and doc/08 plus this report. No CMake/Core/PG/proto/API/doc18 changes.
Not run: complete service/TLS transport integration and revocation/production composition; adapter
unit tests are not evidence those boundaries have been connected. JWT still waits for final Core
credential/session source fields, as requested by parent integration review.
