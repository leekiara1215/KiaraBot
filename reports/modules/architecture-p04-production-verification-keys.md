# P04 production Identity verification keys composition

Scope: Identity production composition; real mTLS-only client under core/tests/platform;
existing owned PG/offline bootstrap/daemon test hooks; directly affected doc02/doc08.
No Core Keys/Issue/Exchange implementation, RPC implementation, crypto, protocol or
protected doc18 edits. Public Key application151f31e and build0d1a321 are already in
the complete reviewed root588119a. The independently committed RPC dependency is83cc837 (local eb042f2). Its fifth
IdentityRpcOptions argument borrows this owned app, preserving issuer/InvocationDeadline
positions. Reviewed Issue4a372eb and RPC fixtureb954c50 are included as dependencies;
the production policy is not bypassed for testing.

The production Applications owner holds an exclusive Keys application borrowing the
same complete PG authority factory and same immutable public-material port from the
single mature DelegationCryptoBundle. No second keyset, copied configuration, alternate
root, authorization wrapper or fake Service is created. RPC shutdown/Wait precedes
Keys application destruction and the shared PG/crypto/material/clock dependencies.
Overall Identity readiness remains false while its complete API is unfinished.

The real client does not register any service and supplies no JWT or idempotency key.
It connects to the production Identity listener with the actual CLI-enrolled Control
certificate. An explicit test enrollment may add identity.keys.read at exactly the
trusted offline Catalog UUID; default CLI test enrollment remains unchanged. The
production JSON contains only the existing trusted key metadata/file references.
No program automatically grants Catalog access merely because a CA is valid.

Fixture cases compare the actual response raw32 Ed25519 public key to the configured
PEM's mature OpenSSL raw projection, and require exact key ID, set/key version and
finite absolute expiry (including nanos zero). Absent, equal and older known version
must return the full set; future/zero are rejected. A second actual owned cluster
case omits the explicit Catalog grant, and the same registered caller must get a
stable denial even for a future version. A CA-valid but unenrolled leaf is also
rejected without key material. All calls use real mTLS, complete RpcFailure decoding
and correlation checks. No generated output, certificate or private file is committed.

Document impact: doc02/doc08 and this report describe production ownership and explicit
Catalog enrollment. The RPC dependency owns its interface/contract documentation;
proto/API/doc06 need no new schema because no HTTP route or wire field is added here.
Build changes are temporary and restored; integrator adds only the Core Keys library
to Identity's existing fixed link list and registers the real client/fixture variants.
Validation actually run:
- Werror production Identity, real Keys client and actual mTLS RPC fixture builds passed
  with no duplicate-library warnings; the client links target_codec transitively for
  proto/gRPC and only adds official OpenSSL include headers (not duplicate archives).
- Owned PG16.4 + offline CLI + production Identity daemon + real mTLS client, explicit
  Catalog grant variant passed: absent/equal/old return exact full material; future/zero
  reject, and unregistered CA-valid leaf is rejected without material. All requests carry
  no JWT and run before any browser Login creates an access token.
- The separate --keys-no-grant owned-cluster variant passed: the actual registered peer
  is denied for absent/equal/old/future known version and never receives key material;
  malformed zero version is rejected as InvalidArgument. The unregistered leaf is also
  denied. Original offline CLI rejection/once-only assertions remain intact.
- Each variant then passed the complete existing production Control HTTPS Login/replay,
  wrong-password rejection, me/list, Refresh/replay and Revoke/revoked401 chain. Every
  owned child was reaped, secret material redaction checked and PG/private files removed.
- CTest p04_identity_rpc (including all previous methods and newly committed Keys
  projection/failure boundary cases) passed. Locked format16, tidy22 two changed C++ TUs,
  Python compilation, dependency boundaries, document local links and diff check passed.
- Complete repository suite is left to the integration owner. No descriptor regeneration
  is needed for this composition-only capability because wire schema is unchanged;
  the independent RPC dependency owns its contract validation. No full readiness claim.

Exact local integration commands use run_database_test.py with the same compiled CLI,
PG binary and Identity/Control paths as p04_identity_daemon_https, plus
--test-argument=--keys-client and --test-argument <compiled-client>. The denial variant
also adds --test-argument=--keys-no-grant. Identity additionally links
kiarabot_identity_verification_keys_application. Client source is
core/tests/platform/identity_keys_daemon_client.cpp; link only kiarabot_target_codec,
add OpenSSL::Crypto INTERFACE_INCLUDE_DIRECTORIES as SYSTEM and normal warnings.
CMake temporary registration is restored before delivery.
