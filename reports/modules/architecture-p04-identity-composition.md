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

## 主线生产接线验收

目标为组合根及构建接线；只更新根 CMake、独立 identity-security.cmake、
工具链文档和本报告。生产协议/错误及配置语义沿用上述独立实现，不改 proto/API
或保护基线。文档影响：doc/11-toolchain.md 需要说明新增实际门禁并已同步；
doc/02-core.md、doc/08-operations.md 已随生产实现说明行为，无需重复修改。

实际执行 cmake configure、完整 full-locked build，以及上述两个 CTest，2/2 通过
（真实 daemon HTTPS 5.18 秒，时钟小于 0.01 秒）。构建日志无 warning/error。
未在本接线提交重复运行全工具链：P04 其他独立能力仍在集成，待一致检查点统一运行。
文档检查使用 git diff --check；未提交构建产物、生成物或密钥。

## Own server TLS chain lifetime

Independent follow-up scope: service_config outer metadata/validation, private clock
and composition, deterministic clock test and production daemon regression, doc02/08.
The existing mature OpenSSL verifier selects the chain; after verification its actual
leaf/intermediate/anchor dates are intersected with checked finite UTC conversion.
JSON/wire/API and Core ports are unchanged. An absent metadata interval cannot build
a configured production bundle. No key/CA signature validation is reimplemented.

The shared security clock latches unavailable on own-chain not-yet-valid/expiry.
It gates each authentication before Admin and again through the real authenticator's
post-check, as well as application/crypto security-clock samples. Credentials readiness
therefore includes own-chain validity. Maintenance returns dependency failure on the
latched fault, and the existing runner exits1 after RPC shutdown/Wait. Established
channels never become a validity cache. Operational failure remains the existing
UPSTREAM_UNAVAILABLE; no new error/status contract is added.

Actually run: Werror Identity/Control/config-driver/clock-test builds, config+no-PG
missing-key canary, deterministic nanosecond nbf-inclusive/expiry-exclusive and invalid
interval tests, native clock with expired credentials and fault persistence, ASan/UBSan,
format16, tidy22 four TUs, boundary checks and Python compilation passed. Actual owned
PG + CLI + daemons with an eight-second real server certificate passed: operations
before expiry, existing Control gRPC channel rejected with503 at expiry, Identity
exited1 and closed RPC, Control stopped normally and cluster/private material removed.
The test's --own-tls-expiry argument selects this additional case; no real-clock change
or fake peer is used. Default regression remains the full five HTTP operation chain.
Doc01/proto/API unchanged because stable failure schema and transport contracts are
unchanged. Protected doc18 remains untouched. Temporary build registration restored
before delivery; integrator should register this second explicit regression variant.

### 主线 own-TLS 门禁和夹具清理

主线新增上述到期 CTest。内部测试清理聚合全部 child 退出结果；超时先终止并收割，
所有 child 清理后才报告失败。敏感日志检查加入真实三把 HMAC/AEAD hex 密钥和
Ed25519 私钥全文。测试基础设施改动没有新业务接口，proto/API/doc06 无需更新。
peer evidence test 移除已由 service-process→config 传递的重复 OpenSSL 链接；没有屏蔽告警。
实际 configure、完整 build 无 warning/error，clock、peer evidence、正常 daemon HTTPS、
实际证书到期 daemon 共 4/4 通过（17.80 秒）。Python py_compile、git diff --check 通过。
工具链文档已同步新门禁；受保护 doc18 不变。没有在局部夹具变更重跑完整发布门禁。

## Explicit verification catalog metadata and shared crypto bundle

This independent configuration/composition capability groups with reviewed public
port A47c23d8 and mature implementation B9cf880f. It is based on complete c83078f
and the previously reviewed production/HTTP/ownTLS capabilities; it does not edit
Core application/ports or mature crypto internals. Allowed changes: outer typed
configuration/parser, production composition, config/daemon regression and doc02/08.
The four exact verification fields and numeric/UUID limits are documented in doc/08.

The loader receives explicit key version and absolute expiry. One DelegationConfig
contains issuer, key and positive set version plus exact offline Catalog anchor;
make_delegation_crypto_bundle returns both mature crypto and immutable public
material. Composition owns both alongside their borrowed clock and validates a
current snapshot before opening the complete PG factory. There is no default
unlimited expiry, key-selected URL, second public key configuration, invented version,
GetVerificationKeys route, or claimed whole-service readiness.

Config tests cover every missing/unknown/duplicate nested field, wrong scalar types,
boolean/float/zero/negative versions/expiry, uint64 version maximum, supported expiry
whole-second maximum, above-range expiry, canonical UUID version/variant/case and
non-UUID strings. The actual production HTTP regression includes valid secure key
files with expired declared key metadata and a TCP canary proving no PG connection
or listener on failure, followed by the full real CLI/PG/Identity/Control chain.

Document impact: doc02/08 and this report updated; proto/API/doc01/doc06 unchanged
because this is a trusted deployment schema and outer ownership change without new
wire/error semantics. Protected doc18 untouched. Build registration is temporary;
integrator adds B verification_keys.cpp to crypto, its unit test with official
OpenSSL headers/assertions, plus the already reported composition targets.

Actually run: Werror Identity/Control/config-driver/public-material/delegation/key-file
target builds, strict config plus missing-key canary, public verification material
binary, delegation/key-file tests, and full production owned-PG HTTP chain passed.
Python compilation, locked format16/tidy22, module boundaries, document links and
whitespace checks passed. No external provider or real platform calls occurred.
The PG application fixture adaptations to B are owned by the PG task and are not
claimed here; A+B+this configuration and that fixture must be integrated together.
