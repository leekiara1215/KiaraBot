# P04 Identity production composition

## Strict configuration declaration

Scope: core runtime/platform service_config and its driver/tests, Identity entrypoint,
doc/02/doc/08 and this report. No App/PG/crypto implementation, CMake, proto/API or
protected doc/18 edits. Input: optional Identity-only identity_security object with
three symmetric key references, Ed25519 public/private file references, issuer and
explicit trusted-clock/skew declaration. Output: typed outer metadata or exit2 for
invalid shape; database/TLS remain mandatory. References are bounded, not secret bytes.

Document impact: doc/08 updated with exact schema/limits/failure behavior and staged
composition behavior; doc/02 with outer ownership. Protocol/API and other module docs
need no change because no wire contract, storage ownership or crypto algorithm changed.
The mature key loader and complete production bundle are subsequent integration, not
claimed by parsing. No security configuration selects the existing not-ready path;
an explicit configuration currently fails composition before DB contact/listeners.

Required reviewed dependencies for the production bundle: 6000920+b538adc secure key
loaders, public mature crypto factories, open_identity_security in security_factory.hpp,
corrected six-method session App, Admin authenticate_peer/bootstrap with persisted
self bindings, DelegationApplication and the final RPC factory with issuance support.
No unsubmitted source is copied. Password365days is the specified trusted finite policy;
existing design only requires an explicit finite policy, so no conflict was found.

Actual declaration checks are recorded below after execution. Full production PG,
bootstrap and RPC assembly remain pending the reviewed companion versions.

Declaration checks actually run:
- cmake --build build/full-locked --target kiarabot_service_config_driver kiarabot-identity -j4 (-Werror): passed.
- PATH=<locked OpenSSL>:$PATH python3 core/tests/platform/service_config_test.py build/full-locked/kiarabot_service_config_driver build/full-locked/bin/kiarabot-identity: passed.
- Shape cases cover every missing/unknown nested key, duplicate IDs/trusted keys,
  wrong owners, missing database, empty/NUL/oversized metadata, boolean/float/negative/
  oversized skew, explicit false trust, and both skew boundaries including integer -0.
- Driver exposes only a boolean declaration-presence bit, proving the typed optional
  is populated without printing paths/key IDs/material. A loopback TCP canary proves
  missing security material does not contact PG or announce listeners.
- Locked format16 dry-run, tidy22 config/main/driver with warnings-as-errors and fixed
  compile DB, boundary checker, document local links and git diff --check: passed.

No full PG/bootstrap/production RPC test is claimed for this declaration-only cap.
The bundle will use parent-directory-trusted file references and secure loaders for
final-component/file permissions and contents, never treat a path as a command/URL.
