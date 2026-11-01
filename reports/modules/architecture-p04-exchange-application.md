# P04 ExchangeDelegation Core application

Scope: new Core exchange application header/source and strict test, doc/02 and this report only.
Clean codex/p04-identity-exchange-application starts from reviewed current root e77fe41, containing
parent crypto port466ba89/concrete3dbd822. Existing Issue/Admin/source credentials remain untouched.
No SQL/JWT implementation, CMake/protocol/config/entrypoint or protected doc18 changes.

Runtime TLS recipient is independently current-validated; the signed parent's previous Control
fingerprint is resolved through authoritative by_certificate/current head/subject/service/SAN/gate.
Never replace the parent fingerprint with Runtime's leaf. Original source credential/session and
authorization version plus exact permissions remain current. Requested tuples narrow the parent,
audience is closed Model/Scheduler, child hop2 appends Runtime and preserves subject/origin.

Mature verify_exchange_parent, random/clock and sign operate outside ALL UoWs. Final DB authority
reread/authorization<=1s, all parent/certificate/source/deadline expiry caps and nonextending integral
NumericDate prevent stale signed results being returned. Pure issuance creates no receipt, command
intent, writes or commit. Role/service identity cannot replace source user grants.

Docs: doc/02/direct report synchronized at delivery; wire contract/API unchanged. Doc/05/08 mature
crypto/provisioning unchanged; doc/21 older example Model/Host does not widen this task's explicit
Model/Scheduler whitelist. Parent owns broader architecture wording reconciliation. Actual checks
and chosen Runtime delegation capability anchor will be recorded after parent decision/tests.

Final parent capability decision (superseding earlier dynamic-business-resource proposal): Runtime
AND previous Control each require CURRENT EXPLICIT IdentityDelegationIssue at their OWN stable
Principal resource (id=actual platform subject). Source subject independently needs each business
permission exact business-resource grant; signed parent subset remains exact. No wildcard/new
Catalog, default role or certificate-as-capability substitute. Stable selfPrincipal avoids requiring
offline service delegation grants for every newly created Session/Task. Existing Issue Control-cap
omission is parent-owned separate hardening, not modified here. Tests distinguish missing/wrong
Principal service grants from source business permissions and post-sign revocation; an old signed
proof cannot bypass current authority.

## Public interface and actual verification

New identity_exchange_application.hpp declares ExchangeIdentityDelegationRequest{context,
VerifiedPeer recipient, SecretView parent_proof, Service audience, vector<PermissionBinding>};
IdentityExchangeApplication borrows transactions/DelegationCryptoPort/RandomPort/SecurityClockPort
and existing IdentityDelegationOptions; exchange_delegation returns existing IdentityDelegationReply.
No existing Issue/Admin headers or source changed. Model/Scheduler only; new API is additive.

Root independently compiled/executed:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -Icore/include \
  core/src/application/target/identity_exchange_application.cpp \
  core/tests/architecture/identity_exchange_application_test.cpp \
  -o build/identity_exchange_application_test
build/identity_exchange_application_test
```

Exit0. Strict public orchestration fixtures trap ordinaryverify, writes/commit/intent/audit, and
assert every parentverify/sign/random/clock call activeUoW0. Both selfPrincipal capabilities are
independently tested missing/wrongPrincipal/businessresource-only/Adminroles-deny; source exact
Model/Scheduler business grants remain separate. Parent audience/hop/subset/origin/actor/cert
bindings and childhop2/lifetime caps tested. Pre/postsign Control newleaf/head/revoke/disable/
SAN/service/expiry refuse while Runtime remains valid; Runtime/source head/session/authorization
changes also refuse. Plugin currentbootstrap origin without humansession succeeds; revokedhead
and missing Human source session refuse. Dependency, untrusted/skewclock, malformed random UUID,
parent/runtimecert/controlcert/sourcecred/session/deadline floor caps, random-gap deadline timeout
and finalUoW>1s timeout all covered. Proof is returned only after final current authority matches.

Fixed tidy22 full clang-analyzer/bugprone warnings-as-errors on source/test with own headerfilter
and explicit macOS SDK/libc++ includes passes without checker exclusions/NOLINT; format16all3files,
boundary/inner/diff pass. Shared existing Identity private authority helpers and frozen state/policy
are reused; no Core SQL/JWT/certificate parsing/algorithms introduced. Previous Control is loaded
from signedfingerprint registration, not fabricated into a current TLS VerifiedPeer.

No real PG+maturecrypto+RPC Exchange integration/fullroot CMake/CTest is claimed: registration and
assembly belong to parent/build/platform. Testcrypto is strict orchestration-only, not a fake
production signature acceptance claim. No service readiness enabled. Changed files limited to new
header/source/test, doc02 and this report; protocols/API/doc05/08/protected18 unchanged. Parent
owns corresponding Issue service-cap hardening and older detailed-design audience example wording.

## Cross-UoW database UTC regression correction

Parent review found final successful authority timestamp was not compared to the initial phase.
Even with all versions unchanged and each individual UoW internally monotonic, DB UTC could regress
between signing phases. Added explicit final.checked_at < initial.checked_at ->UpstreamUnavailable
before any proof response. Does not change existing Issue code or rely on NumericDate flooring.
Strict regression keeps parent/certificate/source valid and final per-UoW time consistent, signs
once, then moves DB time backwards across phases; no signed proof is returned and no writes occur.

Backward-UTC actual regression passes full Exchange -Werror ASan/UBSan no-recover executable:
250ms and1s cross-phase regressions, including initial1000.5/final1000.25/parentnbf1000; each UoW
is internally constant, all authority versions and expiries remain valid, sign1/parentverify1 yet
Failure UpstreamUnavailable and no result proof. Full source/test analyzer+bugprone with no
exclusions, format and diff pass. Root independently rebuilt/executed same test0 before commit.
