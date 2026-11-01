# P04 typed Assignment request fingerprint port

Independent Core port capability after assignment repository f4caa56. New
IdentityAssignmentFingerprintPort::set_assignment(const ExactAssignmentSpec&,optional<AssignmentId>,
optional<ResourceVersion>)->Outcome<Sha256Digest> and
revoke_assignment(const AssignmentId&,ResourceVersion)->Outcome<Sha256Digest> exactly match parent
approved typed boundary. Fixed domains kiarabot.identity.SetAssignment.v1 / RevokeAssignment.v1
are public constants, not Core hashing algorithms. Mature EVP implementation is parent-owned next.

Unambiguous canonical encoding must include method/target optionalmarker+ID/specowner/basis/role
or sortedpermissions/sortedexactresources/expected marker+value. Duplicate/empty/unknown inputs
reject; semantically equivalent ordering yields same digest. Exclude request/trace/deadline/key and
credentials. Domain differs Set/Revoke. Port performs no authorization, DB or network work and
must run outsideALLUoW; caller separately validates authoritative owner kind/current grants and
rechecks authentication before receipt lookup/replay. No client digest trusted or login-scope reuse.

Scope: one port header, minimal compile contract test, doc02/report only. No crypto implementation,
ReplayResult index changes, SQL/proto/API/CMake/main/protected18 edits. This static contract test
proves signatures/domain separation constants only, NOT canonical hash properties or production
crypto acceptance. Set/Revoke application remains disabled until real fingerprint and typed full
success snapshot index14 are integrated behind RegisteredPrincipalSnapshot13.

Actual checks: C++20-Wall-Wextra-Wpedantic-Werror minimal contract compiled/executed0; full fixed
tidy22 clang-analyzer/bugprone warnings-as-errors with ownheaderfilter passed without exclusions,
format16/boundary/inner/diff passed. No runtime canonical/permutation tests yet because no mature
adapter exists in this cap; parent EVP capability must test ordering/duplicates/expected versions
and content changes. No fullroot CMake/CTest registration modified or claimed here.
