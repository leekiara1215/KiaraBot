# P04 public verification material

A: pure Core types/port only. PublicVerificationKey has ref/raw32/positive version/finite expiry;
VerificationKeySet has keys/positive version/trusted offline UUID Catalog resource. Independent
VerificationKeyMaterialPort::snapshot returns Outcome; no old crypto pure class changed. No private
material, PEM/JWT/SQL/transport objects or algorithms enter Core. Mature adapter B follows.

Core application owns current platform mTLS/Identity DB/exact explicit IdentityKeysRead/catalog
checks, without JWT bootstrap prerequisite. Equal/old/absent known version returns full snapshot;
future known version rejects InvalidArgument/CorrectInput. Empty/expired/untrusted material is
UpstreamUnavailable, not an empty success. Single active key only; no rotation manager introduced.

Doc impact: doc/02 and this report updated; proto/API unchanged (existing fields already match),
doc/05/08 unchanged until adapter B. Protected doc/18 untouched. Tests are type contract checks,
not production key parsing/authorization or wire behavior acceptance. No CMake/config/main edits.

A actual checks: C++20-Wall-Wextra-Wpedantic-Werror contract test compiled/executed0; format16,
full tidy22 analyzer+bugprone warnings-as-errors with own header filter passed; boundary/inner/diff
passed. Runtime structural validation and OpenSSL export are adapter B responsibility, not claimed
by pure aggregate construction tests. Full CMake/CTest registration belongs to build integration.

B public signatures: append DelegationKey version/valid_until and DelegationConfig setversion/
keyset_resource; all trusted metadata explicit, zero/infinite invalid. New outer verification_keys
header provides DelegationCryptoBundle{crypto,verification_keys} and
make_delegation_crypto_bundle(DelegationConfig,SecurityClockPort&). First implementation SINGLE
active key, not an implicit rotation manager. The same moved config is mature-validated before
publishing raw32, avoiding independent driftable public config. Old factory also requires metadata;
root must update PGfixtures/composition alongside B, not integrate B alone with old aggregate values.

TrustedDelegationKeyFiles appends version/valid_until; secureloader rejects0/max and preserves both.
Current validity is enforced by trusted-clock factory/snapshot; loader does not fabricate local time.
No config/main/PG/RPC source edits in this capability. Core material port neither authorizes nor
fetches unknownkid. GetVerificationKeys currentmTLS+DB/exactscope behavior remains next application
capability; public snapshot success is not authorization success.

B final actual checks: root independently compiled and executed THREE mature executables
(delegation_crypto_test, verification_keys_test, key_files_test) with C++20-Wall-Wextra-Wpedantic
-Werror-fsanitize=address,undefined-fno-sanitize-recover=all; all exit0. Common includes core/include,
identity_crypto/include and fixed architecture-installed/arm64-osx include as system; links fixed
libsodium.a+libcrypto.a. Sources password_crypto+delegation_crypto for delegation test; add
verification_keys.cpp for projection test or key_files.cpp for loader test. Outputs ignored
build/identity-crypto/*_test, no generated/secret artifacts staged.

Real OpenSSL tests compare publishedraw32 with EVP export, rebuild an Ed25519 PEM from snapshot
and verify an original JWT via verify-only adapter, confirm verify-only cannot sign, exactkeyexpiry
rejects even while proofleeway could otherwise pass, and claims.exp beyondkeyexpiry rejects.
Malformed/zero keyversion/setversion/expiry,empty/multikey,badPEM,missing/all/keys/nil/uppercase/
nonv4/nonRFCvariant Catalog,expired/untrusted/fault clocks all reject without empty snapshot.
Both ordinary and exchange-parent verification still use original strict bindings, now with
trustedkey expiry additionally enforced. Loader explicitmetadata zero/max rejection and exact
version/expiry roundtrip pass, plus all existing secure-file negatives.

Full analyzer+bugprone warnings-as-errors on all6 source/test TUs passes with zero exclusions or
NOLINT, format16all9files,boundary/inner/diff pass. PureA contract also rerun-Werror0. Existing
config/main/PG/RPC callers are NOT modified: old aggregate defaults fail validation and parent MUST
integrate B with their explicitmetadata update. No full CMake/CTest/production RPC claims. No
GetVerificationKeys application/knownversion response behavior implemented in B; those are frozen
Core application responsibilities from A. Doc/05 and this report updated; doc/08 existingtrustedkey
provisioning policy unchanged, metadata config integration follows separately; proto/API unchanged.
