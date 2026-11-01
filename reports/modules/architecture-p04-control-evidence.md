# P04 Control evidence

## Configuration and endpoint ownership

Scope: Control outer transport, service configuration and the explicitly approved
minimal outer endpoint lifecycle. Input: validated ProcessConfig and optional
control_api listen/identity_endpoint. Output: exclusive endpoint start/check/stop;
configuration failures exit 2, local startup/runtime failures exit 1. No protocol,
API, inner application or database implementation change.

Documentation impact: doc/06 and doc/08 updated for listener configuration and
ownership. proto/, api/, doc/01 unchanged: existing wire/error semantics remain
binding. doc/18 is protected and untouched. No other module documentation needs
updating because ownership and behavior outside Control are unchanged.

Actual checks (full-locked uses -Werror):
- cmake --build build/full-locked --target kiarabot_service_config_driver kiarabot_service_composition_driver -j4
- PATH=<locked OpenSSL>:$PATH python3 core/tests/platform/service_config_test.py build/full-locked/kiarabot_service_config_driver: passed
- PATH=<locked OpenSSL>:$PATH python3 core/tests/platform/service_composition_test.py build/full-locked/kiarabot_service_composition_driver build/full-locked/kiarabot_service_rpc_client: passed
- locked clang-format 16 applied to changed C++.

The existing composition driver registration lacks its direct proto link and second
CTest argument on this source base; local temporary CMake links it for verification.
Build owner integration is required; no CMake modification is included in this commit.
Endpoint tests assert start failure, runtime failure and endpoint destruction before
application while dependencies remain alive. Configuration tests cover strict shape,
duplicate keys, unknown values, forbidden services, URL/DSN-like endpoints and ports.
