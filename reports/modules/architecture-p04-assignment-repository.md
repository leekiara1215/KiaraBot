# P04 Assignment state repository interface

Scope: identity_repository.hpp additive typed current(id)/save_state, strict contract test and
existing Core mock compatibility stubs, doc/02 and this report. Branch begins reviewed rootc28bb5f
with frozen1f7d43e assignment-state rules. Existing Issue/Admin production unchanged; no SQL,
CMake/proto/API/crypto/config/protecteddoc18 changes.

current returns optional fullAssignmentStateRecord includingRevoked tombstone for oldscope checks.
for_subject and base grant metadata exposeActive-only from SAME authority. save_state supports
Activecreatev1,Activeupdatev+1immutableowner,Active->Revokedv+1/finiteDBrevokedat; tombstone norestore,
IDreuse/ownererase. Atomic sameUoW base metadata sync; oldtyped save isActive-equivalent and oldbase
save cannot resurrect. CAS/dependency/corruption return existingStatus/Outcome failures, not false
or missing success. Authorization old/newscope union remains application responsibility.

RegisteredPrincipalSnapshot index13/ExactAssignment index14 are parent coordinated: no ReplayResult
change here. Future Set/Revoke app also needs mature typed request fingerprint interface (Set
spec+expectedversion/Revokeexpectedversion); current login-onlycommit scope cannot be reused. This
gap reported to parent rather than implementing Corehash or trustingcallerhash. No app-ready claim.

Actual checks: root independently builds/executes new standalone contract test with C++20
-Wall-Wextra-Wpedantic-Werror-fsanitize=address,undefined-fno-sanitize-recover=all -Icore/include,
output build/identity_assignment_repository_contract_test, exit0. Strict fake+frozen pure helpers
cover Activev1/updateCASv+1 immutableowner/competingCASonewinner, Revoked exact former role/perms/
scope/version/time evidence, forged revokechangedscope/permission/role conflict, Activeonlytyped/
base projections, both oldsave paths noresurrection, dependencyFailure distinct fromempty, atomic
projectionfailure no partialmaps and invalidtimestamp/versionoverflow. This is NOT a PG durability
or adapter acceptance claim. New test full analyzer+bugprone warnings-as-errors has no exclusions;
named validatedspec avoids temporaryvariant analyzer paths without changing frozenheaders.

Existing three Core mocks independently-Werror syntax compiled; existing Admin+Issue and GetKeys
applications/tests rebuilt-Werror and executed0. Existing platform RPC mock only adds failclosed
abort newmethods; full RPC target not rebuilt here (generated transport/dependency/build wiring is
parent/build task). All four compatibility mocks reject unintended newmethod calls rather than
fake success. Format16onport/mocks/newtest, boundary/inner/diff pass. No full CMake/CTest/PG build:
newpure methods must be grouped with real PG adapter update by parent, not root-integrated alone.
Doc02+thisreport updated; unique protocols/API and protected18 unchanged.
