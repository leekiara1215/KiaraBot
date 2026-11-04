# WS-04 target Control WebSocket transport (opt-in by file presence).
#
# Included by the root CMakeLists.txt right after cmake/control-auth.cmake, so the
# kiarabot-control executable and the shared proto/target-codec libraries already exist.
# The target transport keeps a separate WSS worker budget, retained store and Upgrade boundary:
# long-lived sockets never occupy the four REST workers declared by https_endpoint.cpp.
add_library(kiarabot_control_ws STATIC
  ${CMAKE_SOURCE_DIR}/control/api/src/target/websocket_protocol.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/websocket_stream_store.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/websocket_source_client.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/websocket_session.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/websocket_endpoint.cpp)
target_include_directories(kiarabot_control_ws PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
target_link_libraries(kiarabot_control_ws
  PUBLIC kiarabot_control_auth
  PRIVATE kiarabot_proto kiarabot_target_codec Boost::headers OpenSSL::SSL OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_ws)

if(TARGET kiarabot-control)
  target_link_libraries(kiarabot-control PRIVATE kiarabot_control_ws)
endif()

# Pure unit evidence: application framing/limits and the bounded retained store. No TLS/owner.
add_executable(kiarabot_control_target_websocket_test
  ${CMAKE_SOURCE_DIR}/control/api/tests/target_websocket_protocol_test.cpp)
target_link_libraries(kiarabot_control_target_websocket_test PRIVATE kiarabot_control_ws)
kiarabot_platform_warnings(kiarabot_control_target_websocket_test)
add_test(NAME p12_target_websocket_unit COMMAND kiarabot_control_target_websocket_test)
set_tests_properties(p12_target_websocket_unit PROPERTIES LABELS "p12;unit" TIMEOUT 60)

add_executable(kiarabot_control_target_websocket_store_test
  ${CMAKE_SOURCE_DIR}/control/api/tests/target_websocket_stream_store_test.cpp)
target_link_libraries(kiarabot_control_target_websocket_store_test PRIVATE kiarabot_control_ws)
kiarabot_platform_warnings(kiarabot_control_target_websocket_store_test)
add_test(NAME p12_target_websocket_store COMMAND kiarabot_control_target_websocket_store_test)
set_tests_properties(p12_target_websocket_store PROPERTIES LABELS "p12;unit" TIMEOUT 60)

# Real AsyncAPI/OpenAPI schema validation of frames produced by the unit binary (Ajv).
find_program(KIARABOT_WS_NODE NAMES node
  HINTS "${CMAKE_SOURCE_DIR}/build/toolchain/node-v22.14.0/bin")
if(KIARABOT_WS_NODE)
  add_test(NAME p12_target_websocket_schema
    COMMAND "${KIARABOT_WS_NODE}"
      "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/target_websocket_schema.cjs"
      "${CMAKE_SOURCE_DIR}" $<TARGET_FILE:kiarabot_control_target_websocket_test>
      "${CMAKE_BINARY_DIR}/target-websocket-schema")
  set_tests_properties(p12_target_websocket_schema PROPERTIES LABELS "p12;contract" TIMEOUT 120)
endif()

# Real browser TLS -> Control Upgrade -> Identity/owner fixtures WSS acceptance. Uses the locked
# Control acceptance venv (websockets/jsonschema) when present; a missing venv stays a configure
# warning instead of silently registering a test that cannot run.
set(KIARABOT_WS_PYTHON "${CMAKE_SOURCE_DIR}/control/api/tests/.venv/bin/python" CACHE FILEPATH
  "Locked Python used for the target Control WebSocket acceptance test")
if(EXISTS "${KIARABOT_WS_PYTHON}" AND TARGET kiarabot_control_identity_fixture AND
   TARGET kiarabot_control_owner_fixture)
  add_test(NAME p12_target_websocket_https
    COMMAND "${KIARABOT_WS_PYTHON}"
      "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/target_websocket_test.py"
      $<TARGET_FILE:kiarabot-control>
      $<TARGET_FILE:kiarabot_control_identity_fixture>
      $<TARGET_FILE:kiarabot_control_owner_fixture>)
  set_tests_properties(p12_target_websocket_https PROPERTIES
    LABELS "p12;control;integration;fault;security"
    TIMEOUT 180 RESOURCE_LOCK control_https RUN_SERIAL TRUE
    ENVIRONMENT "PATH=${RUNTIME_E2E_OPENSSL_BIN_DIR}:$ENV{PATH}")
else()
  message(WARNING
    "control/api/tests/.venv or Control fixtures missing: "
    "p12_target_websocket_https is not registered")
endif()
