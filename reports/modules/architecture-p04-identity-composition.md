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

## Configured production bundle

The subsequent composition cap replaces the declaration-only startup rejection.
Allowed source scope is core/app/identity_main.cpp and new private composition/clock
helpers plus core/tests/platform/identity_security_clock_test.cpp; no App, PG, crypto,
wire/API or protected baseline changes. Input/output and error schema remain the
existing strict configuration and RPC contracts. Missing/invalid security material
fails startup before PG; configured failures never fall back. Missing declaration
retains the P02 not-ready daemon.

The owned bundle loads three symmetric keys and matching Ed25519 files through secure
loaders, validates explicit clock trust, derives a dummy PHC from 32 random secret
bytes, builds mature crypto, then opens exactly one full security UoW factory.
Session/Admin/issuer applications share that factory. The actual auth_context wrapper
calls Admin.authenticate_peer and all seven implemented RPCs use that same authority.
RPC stops/Wait before application destruction; issuer/Admin/session applications die
before factory/crypto, which die before the borrowed clock. No online bootstrap,
provider call, local store or synthetic peer/binding is introduced.

Clock tests inject pure UTC/steady observations: exact skew boundaries, forward/back
and cumulative drift, permanent fault, overflow, preempted measurement recovery and
regression; native smoke also exercises concurrent sampling. Drift uses the original
baseline and latches. Sampling uncertainty above 1ms is temporarily unavailable without
recalibration. This validates a deployment declaration, not NTP accuracy. Overall
readiness stays false. Fixed finite capacities and lifetime are documented in doc/08.

Document impact: doc/02 and doc/08 updated; proto/API/doc01/doc06 need no changes
because no wire/error schema changed. doc18 is read-only and untouched. No claim of
complete Identity implementation or full production daemon HTTP closure is made by
this source capability. Follow-up verification catalog metadata is a separate cap.

Checks actually run for the bundle:
- Werror build: kiarabot-identity and kiarabot_identity_security_clock_test passed.
- identity_security_clock_test passed; standalone ASan/UBSan clock test passed.
- service_config_test.py with real Identity binary passed, including no-PG missing-key canary.
- Locked format16 dry-run and tidy22 clang-analyzer warnings-as-errors over main,
  composition, clock and clock test passed; module boundary and changed document local
  link checks and git diff --check passed.
- CTest p02_service_config and p03_service_composition passed. p04_identity_rpc could
  not run: the cherry-picked RPC fixture fails Werror compilation because its
  IssueSessionRepositoryFixture lacks count_unexpired and IssueRandomFixture lacks
  opaque_token on this reviewed base. Reported to the parent/author; production
  Identity and RPC library builds passed. This is not treated as a passing RPC test.
- Build registration was temporary and restored before commit. Integrator must add
  the clock static library/include core/app, clock test (Threads and assertions on),
  identity_composition.cpp to Identity, and link Identity with clock/security/admin/
  delegation/RPC/crypto; the new RPC library also links delegation application.

## Actual production daemon HTTP regression

The real owned PG16.4 fixture now invokes a reusable offline bootstrap setup and
production Identity/Control binaries. Control uses the same actual URI certificate
fingerprint enrolled by the CLI, additionally permitted for its browser TLS listener.
The regression covers Login and original-key replay, wrong password 401, current
principal equal to the enrolled administrator, actual session list, Refresh and
original-key replay, successful current lookup, Revoke204 and subsequent401. Both
daemons terminate on SIGTERM, output is checked against actual password/tokens, and
the ownership-checking wrapper removes the temporary cluster and key material.

Actual run: run_database_test.py with identity_daemon_http_test.py and compiled
initializer/bootstrap/Identity/Control binaries passed. Python compilation, module
boundary and diff whitespace checks passed. The bootstrap fixture's default CLI test
path remains unchanged; its new explicit test callback allows the production chain
without source rewriting or mocked authentication. Registration needs the same
arguments as p04_identity_bootstrap plus Identity and Control binary paths. No CMake
changes are committed. This validates seven implemented RPC methods through five
HTTP operations, not the entire Identity API. Own server TLS lifetime enforcement is
a separately identified follow-up and this evidence does not claim it is complete.
