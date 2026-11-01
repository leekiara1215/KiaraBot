# P04 full original Assignment success snapshot

Newbranch codex/p04-assignment-result starts current reviewedroot06a0d87 plus parentapproved
registration15fd119 cherry dc00e54. Merge preserved priorSessionMetadata12 and all oldresulttypes;
RegisteredPrincipalSnapshot fixed13. This cap appends ExactAssignment EXACTLY14 with explicit
identity_types include, no index reuse/removal. No ordinarysecretpayload or live grant lookup.

Scope: persistence.hpp, new strictsnapshotcontracttest, doc02/report only. Tests statically assert
ALL0..14 variants and count15; SecretReplayPayload still not constructible as ReplayResult. Runtime
copy check preserves owner/version/explicitexacttuple or rolebasis original after live objectmutates.
This is immutabletypedCore result contract, not serialization/PGdurability or authorization tests.
PGauthor must provide typed codec/schema grouped; application currentauth still precedes replay.

No SQL/proto/API/CMake/main/crypto/protected18 edits. RegisteredPrincipal13 dependency is separate
parent commit; do not reapply localcherry dc00e54 if root alreadyhas15fd119. No state/interface
fingerprint implementation changed here, and no Set/Revoke app routes enabled.

Actual checks: C++20-Wall-Wextra-Wpedantic-Werror-ASan/UBSan-no-recover newAssignment snapshot
test built/executed0; existingRegisteredPrincipal13 snapshot test independently-Werrorbuilt/ran0.
Fullfixedtidy22 analyzer+bugprone warnings-as-errors on newtest with ownheaderfilter, format16,
boundary/inner/diff pass without exclusions. No full CMake/CTest/PGcodec build here; root should
integrate this append with PGtypedcodec before generic receipt paths use newtype14.
