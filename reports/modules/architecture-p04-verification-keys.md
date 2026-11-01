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
