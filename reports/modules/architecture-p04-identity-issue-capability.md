# P04 IssueDelegation explicit platform capability

Scope: Core IdentityDelegationApplication, strict existing Admin/Issue application
unit fixture, actual PG application fixture and offline-bootstrap test enrollment,
doc01/02/08 and this report. No Exchange/crypto/ports/protocol/CMake/baseline edits.
Input is the unchanged Issue request: actual current Control peer, original access
proof, requested audience and exact business permission tuples. Output remains a
short delegated proof only after pre/post current authority checks. Error mappings
are unchanged stable PermissionDenied/Unauthenticated/Conflict/Timeout/dependency
failures; no new enum or wire field is introduced.

Final task adjudication: service delegation capability is anchored on the actual
Control service's own stable Principal, not on each dynamic business resource.
IdentityDelegationIssue must be explicit. The represented subject's requested
business tuples are independently authorized exactly as before. No role, Catalog,
certificate, other Principal or cross-assignment scope implies the service capability.
This avoids requiring an offline CLI to predict future random Session identifiers.

Implementation reuses identity_security_detail::begin/read_peer/check_peer/
read_subject/final_check, retaining current source-session and business permission
checks. Short authority reads are released before clock/random/sign/session ports.
After signing, source validation and a new authoritative capability read must succeed;
Control credential and authorization version must equal the pre-sign snapshot. Each
checkpoint is bounded to one second and relevant request/credential/certificate
expiry. Backwards authority time is a dependency failure. Final checked_at comes
from the final authoritative read, not a locally invented timestamp.

No reusable proof is returned on failure. The real PG and offline CLI fixtures now
explicitly enroll IdentityDelegationIssue at actualControlPrincipal. The CLI remains
a consumer of user-supplied grants, and does not silently create any capability.
Existing source-self session rules and all old positive/negative/bounds assertions
remain; new strict cases cover missing explicit cap, role-only, wrong human Principal,
Catalog substitution, revoked Control credential, grant removal/version change,
credential rotation/expiry, disabled service, SAN mismatch during signing, pre/post
one-second checkpoint failures and no signing before an initial refusal. Scripted
crypto still asserts zero active UoWs; no proof is persisted.

Document impact: direct contract/module/operations docs updated to distinguish the
two authorization layers and stable refusal semantics. proto/API/doc06 require no
schema or transport changes; the existing permission and errors already exist.
Protected doc18 is read-only and untouched. Exchange remains owned by its author.
Actual commands/results are recorded after execution below.

Actual validation:
- Werror build: kiarabot-identity, kiarabot_identity_admin_application_test and
  kiarabot_identity_application_test passed; final updated strict unit rebuilt/passed.
- CTest p04_identity_bootstrap, p04_identity_admin_application and
  p04_identity_application: 3/3 passed with owned PG16.4 and mature crypto.
- Production owned-PG bootstrap/Identity/Control HTTPS regression: passed all five
  operations, original-key replay and revoked-session rejection, normal shutdown,
  redaction and private fixture cleanup with explicit own-Control capability.
- Standalone -Werror ASan/UBSan Admin/Issue strict unit passed, including backwards
  post-sign authority time refusal. Format16, tidy22 all three changed C++ TUs
  (updated unit repeated), Python compilation, boundaries, changed document local
  links and git diff --check passed.
- Full repository suite was not run: focused targets cover this isolated Core behavior;
  integration owner runs the grouped source suite. RPC Issue fixture needs its owner's
  independent explicit Control authstate/capability fixture update and is not claimed
  passing under the newly tightened policy here. No protocol/API schema changed, so
  descriptor regeneration is not applicable. CMake registrations restored, no keys,
  generated files or host config committed.
