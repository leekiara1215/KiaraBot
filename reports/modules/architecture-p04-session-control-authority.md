# P04 original Control current session authority

Base: reviewed root4f21653 (Login081/type878/schema7), with production Keys a50b002
(local240f3e8). Scope: Core identity_security_application.cpp/private detail helper,
its original strict unit, doc01/02/08 and this report. No Keys/RPC/Exchange or
RevokeCredential implementation, storage schema, protocol or protected doc18 edits.
Input/output types remain unchanged. Existing stable Unauthenticated rejects missing,
mismatched or no-longer-current original Control; dependency faults stay failures.
No fallback to arbitrary Control and no certificate-as-authority substitution.

Shared private signatures for other authors:
- read_session_control(IdentitySecurityUnitOfWork&, const SessionSecurity&, TimePoint)
  -> Outcome<PeerAuthority>: current typed original Control, never fake TLS evidence.
- require_session_control_peer(const SessionSecurity&, const VerifiedPeer&) -> Status:
  actual current Control stable subject/credential ID match, not certificate-version pin.
- session_control_valid_until(const PeerAuthority&) -> TimePoint: current cert/credential
  upper bound for final_check.
- read_session adds an explicit-now overload; the original two-argument form remains
  and reads authoritative current_time. SessionAuthority owns its original Control snapshot.

All access/refresh/replay reads recheck the original Control. Current actor source_session
requires actor_chain.front to equal its persisted original Control and checks that current
registration separately from the actual direct Runtime. Existing current source, business
permission and transport checks remain. Final validity includes original Control TTL and
one-second security checkpoint. All crypto remains outside live UoWs. No receipt/token
success is cached and no new binding is synthesized on old records.

Strict tests preserve the original assertions and add a different currently registered
Control rejection for access/refresh, same-ID certificate rotation through validate/login
replay/refresh/replay, valid direct Runtime with original Control followed by disabled/
expired/revoked/missing binding/wrong origin/dependency refusal, and post-token-crypto
missing binding/disabled Control/dependency failures. Scripted ports remain explicitly
unit fixtures; real PG/HTTPS checks are recorded below after execution.

Document impact: doc01/02/08 updated for this directly observable session authorization
behavior. proto/API/doc06 unchanged: persisted type/PG schema are prior reviewed caps,
and no new field/error/HTTP mapping is introduced. Protected doc18 is untouched.

Actual validation:
- Complete dependency set includes reviewed83cc837 RPC (localf347a19), required by
  the prior Keys composition; no RPC source changes are in this capability.
- Werror Identity/CLI/initializer/full SessionApp unit and actual PG application builds
  passed after all artifacts were relinked against the same schema7/port headers.
- CTest p04_identity_security_application and p04_identity_application: 2/2 passed;
  original full strict unit groups retained. Latest strict Login/security group includes
  separate same-subject/different-credential-ID denial and an independently authenticated
  Runtime proving original-Control expiry at the final +500ms boundary rolls back Revoke.
- ASan/UBSan Werror strict Login/security group passed with all new cases.
- Real owned PG16.4 + CLI + schema7 production Identity/Control HTTPS chain passed
  Login/replay, invalid password, me/list, Refresh/replay, Revoke and revoked401;
  normal shutdown/redaction and cluster/private-file cleanup passed.
- Format16, tidy22 both changed C++ TUs including private helper, module boundaries,
  changed document local links and git diff --check passed. Full repository integration
  suite remains with the integration owner; no wire schema changed, so no descriptor
  regeneration applies.

An initial local HTTP attempt mixed a stale CLI artifact after an earlier multi-target
build stopped at the missing prior RPC dependency; it returned INTERNAL before any
session path. The dependency was included and own ignored PG objects/binaries rebuilt;
the complete same-root run above passed. No SQL or other authors' implementation was
changed to work around that local build issue. Temporary CMake addition only links
Core Keys for the prior source cap and is restored before delivery.
