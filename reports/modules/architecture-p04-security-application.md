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
