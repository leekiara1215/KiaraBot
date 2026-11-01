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

## Delegation production adapter acceptance

Core source fields cherry-picked from reviewed b0949b3 as 592cf1e. New public delegation factory
consumes immutable issuer/Ed25519 keys and borrows a signing SecurityClockPort. Source credential
and optional source session are included in the signed closed delegation object; application
preserves origin on exchange and checks current revocation/authorization independently. No DB
lookup, CA framework, dynamic key URLs, arbitrary algorithms or local-wallclock substitution.
Strict bounded full PEM parse validates Ed25519 matching keypairs; verify-only keys are supported.
NumericDate is nonnegative integral seconds, lifetime<=56s, known skew<=2s and leeway<=2s.
The direct peer must have a fresh verified_at, valid registration time, matching subject/service
and fingerprint. Lists/strings are bounded; duplicate actors and exact permission/resource pairs
are rejected. Only alg/kid/typ JWT headers and the exact expected claim set are accepted.

Real jwt-cpp0.7.0/OpenSSL3.5 Ed25519 test passes roundtrip including UINT64_MAX versions and origin
preservation. Negative paths: wrong signature/audience/issuer/key/none/jku; untrusted/too-skewed
clock, stale/incorrect peer/cert; zero/wrong-type/overflow source; fractional/huge/wrong-type dates;
expired proof, duplicate permissions and max/min time overflow edges. A real UBSan finding in
upstream JWT NumericDate conversion was prevented by strict bounded type/shape parsing before
mature signature verification; no result is trusted before signature verification succeeds.

Actual command from operations worktree uses the first-stage compiler flags plus
-fno-sanitize-recover=all, compiles src/password_crypto.cpp src/delegation_crypto.cpp and
tests/delegation_crypto_test.cpp under infrastructure/identity_crypto, includes core/include and
identity_crypto/include, marks fixed-prefix/include as system, links fixed-prefix/lib/libsodium.a
and libcrypto.a, outputs build/identity-crypto/delegation_crypto_test; execution passes.
Format16, boundary/inner and diff checks pass. Tidy22 analyzer/bugprone checks pass with explicit
SDK/libc++ flags and header filter for identity_crypto, EXCEPT one deliberately excluded check:
clang-analyzer-cplusplus.NewDelete reports a third-party picojson.h:233 claim-copy destructor path
through jwt-cpp set_header_claim. Full-check attempts and named claim/builder alternatives still
report it; ASan/UBSan real paths pass. No third-party file or broad repository check configuration
was modified. This is a remaining static-analysis limitation, not a claim of full tidy acceptance.

Updated doc/05 and doc/08 describe key provisioning, clock and origin responsibilities. Protocol,
API, Core interfaces and protected baseline unchanged by this adapter commit. Not run: CMake/full
CTest registration, live TLS/RPC/DB integration or deployment; build and service assembly belong
to parent tasks. Existing four adapter tests were rebuilt against 592cf1e with ASan/UBSan/-Werror
and all passed. No production readiness or current authorization decision is claimed here.

## Full static-analysis and hop interoperability correction

Supersedes the preceding Delegation tidy limitation: switched only the official JSON traits of
fixed jwt-cpp0.7.0 to its nlohmann-json integration using the already locked nlohmann dependency.
No JWT library/version, claim schema or crypto algorithm changed. Full clang-analyzer-* and
bugprone-* with warnings-as-errors now pass for source and test WITHOUT excluding NewDelete,
without NOLINT, and without editing unified checker configuration or third-party code. The old
picojson lifetime-path diagnostic is therefore avoided rather than asserted to be harmless.
Raw NumericDate requires integer type and unsigned<=INT64_MAX before conversion, then a bounded
nanosecond timestamp; authenticated UINT64_MAX, huge float and fractional date tests reject.

Parent/Core confirmed business edges: Issue Control→Runtime is hops1/chain[Control], Exchange
Runtime→Model or Host is hops2/chain[Control,Runtime]; Identity RPC calls themselves do not count.
Adapter now requires hops1..2, chain.size()==hops, last actor==direct_actor and no duplicates.
Zero/three hops or mismatched chain reject. Core owns exact child chain append/new audience,
permission narrowing and preserved origin; one signed proof cannot establish parent relation.
Doc/05 synchronized; doc/08 key/clock behavior unchanged by this correction, proto/API/doc18
unchanged. Real Ed25519 tests cover the first and second edges and explicit rejected extra edges.

## ASCII opaque token follow-up

Reviewed pure RandomPort extension ef956c6 cherry-picked as c6a2da2; doc/02 append conflict retained
both reviewed additions. SodiumRandom::opaque_token draws32bytes CSPRNG into sodium_malloc memory,
uses mature sodium_bin2hex into secure65-byte allocation, exposes only64lowerhex bytes with no NUL.
Raw bytes remains for internal UUID entropy. Both entropy and output erase through sodium_free;
no stdstring plaintext conversion or Core encoding/wiping. Allocation exceptions map to existing
UpstreamUnavailable; no weak fallback. Deterministic allocation-fault injection was not added to
production allocator API, so that rare branch is code-reviewed rather than runtime fault-tested.
Real64hex/noNUL/different-output tests and existing password tests pass with -Werror ASan/UBSan
-fno-sanitize-recover=all. Full tidy22 analyzer/bugprone on source+test passes with zero exclusions;
test main now catches unexpected exceptions. Format16 and diff checks pass. No CMake/main/API
changes; parent must integrate pure header and this implementation together to avoid abstract
RandomPort subclasses. Frozen six-method application work remains separate and in progress.

## Trusted key file loader acceptance

Scope: new key_files.hpp/src/key_files.cpp/tests/key_files_test.cpp plus doc/05/08 and this report.
Inputs are trusted path/reference metadata; outputs existing SymmetricKey/DelegationKey, including
verify-only public key loading. No ServiceConfig/Core/composition-root/API/proto/PG changes.
Symmetric input exactly64 lowercase hex plus optional single LF -> sodium_hex2bin -> secure32bytes.
PEM input at most8192bytes each, strict full parse/type/Ed25519/pair equality. Secret source bytes,
hex source/decoded result/private PEM use sodium_malloc/free storage; no secret content logging.
O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC precedes fstat regular validation/read. Secrets require effective
UID and no group/other bits. Before/after metadata compares size/type/owner/mode/device/inode and
mtime/ctime; extra-byte read detects growth. Parent directories must be trusted. No write/create,
permission repair, generation, rotation, CA or database behavior. Invalid input/material/perms/types
return InvalidArgument; syscall/resource failures UpstreamUnavailable; no diagnostic path/content.

Actual operations-worktree command: clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror
-fsanitize=address,undefined -fno-sanitize-recover=all -Icore/include
-Iinfrastructure/identity_crypto/include -isystem <fixed-prefix>/include compiles
infrastructure/identity_crypto/src/{password_crypto,delegation_crypto,key_files}.cpp and
infrastructure/identity_crypto/tests/key_files_test.cpp, links <fixed-prefix>/lib/libsodium.a and
libcrypto.a, writes build/identity-crypto/key_files_test; executable passes. Fixed-prefix remains
the authorized shared toolchain architecture-installed/arm64-osx location above. Full tidy22
--checks='-*,clang-analyzer-*,bugprone-*' --warnings-as-errors='*' on loader source/test, explicit
SDK/libc++ includes and own adapter header filter passes without individual exclusions/NOLINT.
Format16, boundary/inner and diff checks pass. Temporary fixtures stay under ignored build paths
and are cleaned by tests, never committed. No secrets/generated artifacts/local config committed.

Real tests: owned0600 valid hex/rawLF, exact bytes32, malformed hex/uppercase/NUL/extra newline,
empty/oversize/mode0640/FIFO<=1second/symlink/directory/missing/NUL path, actual OpenSSL Ed25519
PEM pair/verify-only, private permissions/oversize/NUL/public symlink, mismatched pair rejected by
both loader and existing factory. Not tested: foreign-owner chown (no privilege modification),
concurrent file-drift injection (no production hooks), Linux runtime, full CMake/service/deployment.
Those limits do not remove implemented metadata checks. Doc/05/08 synchronized; doc/01/02 and
proto/API unchanged because this is outer trusted loading with existing port semantics. Protected
doc/18 remains untouched; root build registration and composition remain with parent/build task.

## Public key trust-file permission correction

Review found secret=false file metadata previously allowed writable/unowned trust material. Now
public PEM requires owner effective UID or root and mode&022==0 before AND after read; regular,
size, nofollow and drift checks remain. Real owned0644 positive and0664/0666 InvalidArgument tests
pass, plus all loader negatives, using the same -Werror ASan/UBSan no-recover command above.
Secret euid/no-group-other rule unchanged. No foreign-owner/root fixture chown attempted; ownership
predicate reviewed, not falsely claimed runtime-tested. Doc/05/08 synchronized, ports/API unchanged.
