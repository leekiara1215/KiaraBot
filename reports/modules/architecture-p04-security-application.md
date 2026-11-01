# P04 Identity six-method security application

Scope: core/src/application/target/identity_security_application.cpp, private module helpers,
core/tests/architecture/identity_security_application_test.cpp, doc/02 and this report. Frozen
public header a11716c is cherry-picked as d37e49f. No infrastructure/PG/API/proto/CMake/entrypoint
changes are authorized here. Existing P02 make_identity_application remains untouched.

Inputs: frozen Login/Refresh/Access/ActorProof and page requests, trusted bounded options, borrowed
security UoW and mature crypto ports. Outputs typed move-only tokens or current metadata; errors
use existing Outcome/Failure with acceptance/recovery. Current principal/credential/session/peer
and DB time govern authentication, before receipt replay. Unknown commit returns no secrets and
requires recovery with original identity/key; no automatic operation retry or seven-day plaintext
receipt substitution. Expensive crypto never runs with an active UoW.

Documentation impact: doc/02 must describe actual application state/transaction/replay/snapshot
behavior as implemented. Protocol/API, infrastructure and operations docs require no change for
this inner implementation using frozen interfaces; no new configuration or network endpoint is
introduced. Protected doc/18 remains read-only. Reviewed policy/state rules are integrated from their own author as recorded below; no
uncommitted rule implementation was copied.

Planned acceptance: -Werror build with production source and strict memory transactional test ports,
ASan/UBSan, format16, full tidy22, boundary/inner and diff checks. Tests enforce no crypto inside
active transactions and exercise authentication, rollback/Unknown, refresh recovery and snapshots.
These are application unit tests, not claims of real PG/crypto/RPC integration or service readiness.

## Frozen dependencies and initial linked verification

Reviewed policy/state rules5a1f5eb cherry606b5d1; complete session receipt a7d5d72+b45241d
cherry021bcdd+18a1c27. Reviewed ASCII token portef956c6 cherryc6a2da2 and sodium implementation
9d3204e precede this application. Doc/02 append conflicts retained reviewed additions.

First complete six-method implementation linked to strict transactional test ports. All grouped
runtime tests pass with C++20 -Wall -Wextra -Wpedantic -Werror and ASan/UBSan
-fno-sanitize-recover=all. Tests assert activeUoW==0 for every crypto/clock/random call, exercise
Unknown persisted results without secrets/retry, rollback/audit failure, current credential before
receipt open, exact old refresh recovery/newkey rejection, immutable page/current authversion and
revoke original metadata replay despite live row changes. Rate tests initially covered the proposed5userpeer/60peer,
unknown-user dummy cost,4096global keys/no active eviction,monotonic expiry and restart reset.
Final precommit timing/state consistency and refusal auditing are completed as recorded below.

Parent corrected the earlier example to retain the unique API limit:30 per normalized username
globally across peers, plus60 per peer local protection. Tests/source are updated to this value.
The API global4096unexpired-session cap requires the parent's forthcoming count_unexpired port;
per-subject options remain an additional bound, not a substitute for the global cap. No API files
are edited by this inner application task. Root tests are not PG/RPC/production composition claims.

## Final authoritative capacity and consistency gates

Reviewed count_unexpired port ba8e56c cherry3ac84e1 supplies actual same-UoW DB UTC global count
and constant4096. Login final transaction checks existing receipt first, then global count, then
per-subject bound. No absence/failure-to-zero fallback or cross-subject scan. Expired/revoked
sessions do not count; full capacity does not block original successful receipt replay.

Current-state tightening includes owned credential.binding equality, exact base/security metadata
parity, login password verifier/subject/head reread, username-reassignment receipt rejection,
decrypted token verifier/expiry consistency and final state reread. Rejection audit derives current
registered direct-peer authorization version, never invented0 or unverified represented identity;
missing trustworthy registration preserves the original denial without fabricating attribution.
Success and mutation audit failures roll back; Unknown never becomes a new operation/audit retry.
Refresh updates authorization version/dirty marker atomically with rotation/consumption/receipt.
All writes and sensitive returns perform fresh DB-time deadline/expiry checks; actor checks-to-local
commit greater than1second abort. Monotonic limiter regression fails closed even after pruning.

## Final checks and delivery limits

Actual independent root verification from operations worktree:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -Icore/include \
  core/src/application/target/identity_security_application.cpp \
  core/tests/architecture/identity_security_application_test.cpp \
  -o build/identity-crypto/identity_security_application_test
build/identity-crypto/identity_security_application_test
```

Passed all groups including capacity. Test supports login/refresh/revoke/list/rate_capacity and
all(default) groups. Concrete tests include global4096and4097refusal, originalsamekey replay atcap,
revoked/expired exclusion and counterfailure no writes. Core pure rules also independently rebuilt
-Werror and executed successfully after reviewed dependency integration.

Fixed clang-format16 --dry-run --Werror passes on production/helper/test. Fixed tidy22 on source
and test with --checks='-*,clang-analyzer-*,bugprone-*' --warnings-as-errors='*', own application
header filter and explicit macOS SDK/libc++ include flags passes with NO checker exclusions/NOLINT.
Nonuser/Core-header diagnostics outside this application's filter are not claimed as newly fixed.
python3 scripts/quality/check-boundaries.py and check-architecture.py --inner pass; git diff --check
passes. Existing root .clang-format groups headers differently from doc/12; localized include
format guards preserve corresponding-header/std/internal order without unrelated repo reformatting.

Only production application/private helper, its test, doc/02 and this report are this capability.
No generated files, build outputs, secrets, machine config or protected baseline changes are staged.
No new CMake/main/transport/PG implementations here. Root CMake/full CTest/check-toolchain and real
PG+mature-crypto+TLS/RPC integration are NOT run in this isolated capability: build registration
and production assembly belong to parent/build/PG tasks. Strict test doubles establish application
sequencing and failure behavior, not production crypto or database durability. Readiness remains
false with database/credentials Unverified; no six-service production readiness is claimed.

Documentation checklist: doc/02 updated for inputs/outputs/error/replay/limits/transaction timing;
this new report records actual checks. doc/01/proto/API and doc/05/08 need no inner-interface change
because frozen protocols/ports and existing API30username+4096sessions are preserved. Parent owns
unique contract clarification for additional local peer60CPU protection. doc/18 is untouched.

## REST-compatible opaque continuation correction

Review identified frozen REST returns/passes only page_token while initial application required an
additional snapshot_id. Continuations now use a bounded server-side token->{snapshot,page} index,
not an untrusted client offset or full snapshot scan. Optional explicit snapshot_id is checked
against the indexed snapshot; no new public REST fields. Snapshot prune removes token index too,
and insertion exception rollback preserves bounded counts. Current authorization and complete
SnapshotOwner bindings still apply before and after cached page copy.

Real linked list-group -Werror ASan/UBSan test now continues using ONLY the returned token; rejects
tampered/expired/restarted token, mismatched explicit snapshot, changed valid actor/origin/scope,
page size and jointly updated DB/proof authorization version. Existing explicit-ID path remains
compatible. Full analyzer/bugprone and format checks on source/helper/test pass without exclusions.
Only application/helper/test, doc/02 and this report belong to this correction; no API change.

## Repository-owned base metadata synchronization correction

Real PostgreSQL review found security repository save already synchronizes base sessions row;
initial test double omitted this contract and let the application duplicate insert/CAS. The port
comment now explicitly assigns BOTH security and base atomic writes to session_security.save.
Login/refresh/revoke remove their extra sessions.save calls. Strict memory fake now automatically
synchronizes base metadata in its transactional snapshot and rejects independent application base
writes; assertions require zero such writes. CAS/rollback/Unknown and metadata parity tests remain.
No PG implementation is edited here; this clarifies the existing mature PG adapter responsibility
rather than inventing independent transactions. Read parity remains an application corruption gate.

Base-sync final validation: actual production+strictfake all-groups C++20-Werror ASan/UBSan
-fno-sanitize-recover=all passed. Internal fault between security staging and base synchronization
poisons the test UoW and rolls back both rows plus consumption/receipts/audit/invalidation; explicit
login/refresh/revoke faults verify this. Every Fixture asserts direct base writes==0. Full test and
production analyzer/bugprone with warnings-as-errors and zero check exclusions passed; format16,
boundary/inner and diff passed. Parent PG author independently reproduced duplicate base writes
as Conflict+wholetransaction RolledBack in87c0fa7; no PG implementation/constraints weakened here.
Actual real App+crypto+PG fixture integration remains with parent/PG author, not claimed by fake.

## Login index reassignment replay correction

Independent Control reproduction found that a username index reassigned after password verify
could still replay the prior subject's valid receipt. Login now carries its initially authenticated
LoginIdentity expectation into EVERY replay gate and re-reads exact normalized login, subject,
password credential and index version before and after AEAD. Receipt/session validity alone is
insufficient. Refresh recovery does not invent a username binding. Regression hooks reassign to a
real other Human/current credential after password verify and during AEAD open; no old-subject
secret may be returned and the first case must reject before decryption. No secret/username audit
fields are introduced. Current proof/token expiry is not extended by this correction.

Issue1 checks: production Werror object, source/helper and test full analyzer/bugprone with zero
exclusions, format16 and diff pass. Actual linked login group ASan/UBSan no-recover passes six
adversarial cases: predecrypt after_verify and postdecrypt after_open hooks, each with real other
owner+credential reassignment, version-only change and normalized-name corruption. Early paths
perform no AEAD open; late paths perform one open but return no secret/new session. No issue2-4
changes are mixed into this commit.

## Same-key refresh final-window race correction

Independent reproduction commits a same-key refresh between the last read_refresh release and
final begin. The stale Current handle previously made the outer call reject despite an original
successful receipt. Final UoW now re-resolves exact input/peer/key and receipt from current state;
if consumed by that key, releases the UoW before normal replay/AEAD. No automatic second rotation,
no new-key history lookup, and current head/session/rotation/expiry gates remain authoritative.

Issue2 real deterministic before_begin third-boundary regression passes -Werror ASan/UBSan
no-recover refresh group: a one-shot reentrant competitor commits first, outer returns byte-identical
original access/refresh/session/rotation, exactly one consumption/receipt/commit and no second
rotation. Old input with new key remains Unauthenticated. Every crypto call asserts no active UoW.
Production/test full analyzer+bugprone, format16 and diff pass without checker exclusions.
