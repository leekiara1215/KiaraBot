# P04 GetVerificationKeys Core application

Scope: new application/target/identity_verification_keys_application.hpp/.cpp and matching Core
application test, doc/02 and this report. Branch codex/p04-identity-key-application starts from
reviewed fullroot c83078f; preceding cryptoB branch is retained. No SQL/proto/API/crypto/config/
CMake/entrypoint/protected doc18 edits. AGENTS/coding/boundary rules unchanged across bases.

Material.snapshot executes outside ALL UoWs. The application validates public-only single-key
material shape/version/finiteexpiry/Catalog UUID, then reads current registered platform peer,
credential head/subject/certificate/SAN/service and exact IdentityKeysRead at material catalog in
one short Identity UoW. No JWT prerequisite, no network/cache or write. Final current state/version
and DB time/expiry/<=1s authorization checkpoint precede response. Future known_version checked only
after authorization; zero is invalid input, same/old/absent returns complete original set.

Doc impact: doc/02 and this report describe implemented Core behavior. Existing wire fields/error
contract already match; no proto/API changes. Doc/05/08 crypto/provisioning unchanged; config and
real transport assembly remain separate parent tasks. Unit fixtures contain only public key bytes,
not real crypto validation or PG integration claims. Actual checks recorded after implementation.

## Actual checks

Root independently compiled and executed:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -Icore/include \
  core/src/application/target/identity_verification_keys_application.cpp \
  core/tests/architecture/identity_verification_keys_application_test.cpp \
  -o build/identity_verification_keys_application_test
build/identity_verification_keys_application_test
```

Exit0. Strict script UoW covers normal Control/Runtime, exactCatalog permission/no role or child
inheritance, absent/old/same fullkeyset, future/zero versions and authorization-before-future,
current cert/SAN/service/principal/head/credential invalidity, betweenphase head/authversion/grant
changes, materialbadshape Internal, material/DB/begin/time faults, finalUoW>1s Timeout, key expiry
at final DB500ms UpstreamUnavailable and checked_at fresh500ms. Slow material access before fresh
final authorization may complete within request deadline; it does not start the final1s checkpoint.
All fixtures assert no commits/writes/unexpected repository use; material asserts activeUoW0.
The publicraw32 fixture is not claimed as mature crypto/keypoint validation (adapter capability B
owns it). No private data or real SQL/service calls in these application tests.

Full fixed tidy22 --checks='-*,clang-analyzer-*,bugprone-*' --warnings-as-errors='*' on source/test
with own application header filter and explicit macOS SDK/libc++ flags passes with zero exclusions
or NOLINT. Format16all3files, boundary/inner and gitdiffcheck pass. Existing shared private Identity
helper is reused unchanged; no duplicate credential/authorization rule implementation. Catalog
check is canonical UUIDv4 text shape, not Core PEM/cryptographic parsing.

No root CMake/fullCTest/check-toolchain or real material+PG+mTLS/RPC composition test was run:
registration/assembly belong to parent/build/platform tasks, not authorized here. No service-ready
claim is made. Only header/source/test/doc02/thisreport changed; doc05/08, frozen protocols/API and
protected18 are untouched. Unknown-kid bootstrap has no JWT prerequisite, but still requires true
current platform mTLS and explicit exact authority in the Identity DB.
