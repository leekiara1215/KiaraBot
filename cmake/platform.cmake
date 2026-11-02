# P02 production libraries never inherit the historical reference include directory.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
find_package(Threads REQUIRED)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

function(kiarabot_platform_warnings TARGET_NAME)
  target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)
  target_compile_options(${TARGET_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
endfunction()

# Wire conversion belongs to the outer layer; pure application libraries never link it.
add_library(kiarabot_target_codec STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/target_codec.cpp)
target_include_directories(kiarabot_target_codec PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_target_codec PUBLIC kiarabot_proto)
kiarabot_platform_warnings(kiarabot_target_codec)
add_executable(kiarabot_target_codec_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/target_codec_test.cpp)
target_link_libraries(kiarabot_target_codec_test PRIVATE kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_target_codec_test)
add_test(NAME p04_target_codec COMMAND kiarabot_target_codec_test)
set_tests_properties(p04_target_codec PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)

# Descriptor-driven authorization policy resolver: canonical proto method metadata only, no
# handwritten method table. Unannotated/unknown methods resolve empty and callers fail closed.
add_library(kiarabot_authorization_method_policy STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/authorization_method_policy.cpp)
target_include_directories(kiarabot_authorization_method_policy PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_authorization_method_policy PUBLIC kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_authorization_method_policy)
add_executable(kiarabot_authorization_method_policy_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/authorization_method_policy_test.cpp)
target_link_libraries(kiarabot_authorization_method_policy_test PRIVATE
  kiarabot_authorization_method_policy kiarabot_proto)
kiarabot_platform_warnings(kiarabot_authorization_method_policy_test)
add_test(NAME p04_authorization_method_policy COMMAND kiarabot_authorization_method_policy_test)
set_tests_properties(p04_authorization_method_policy PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)

foreach(SERVICE identity runtime plugin_host scheduler model)
  set(APPLICATION_SOURCE "${SERVICE}")
  if(SERVICE STREQUAL "plugin_host")
    set(APPLICATION_SOURCE host)
  endif()
  add_library(kiarabot_${SERVICE}_application STATIC
    ${CMAKE_SOURCE_DIR}/core/src/application/target/${APPLICATION_SOURCE}_application.cpp)
  target_include_directories(kiarabot_${SERVICE}_application PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
  kiarabot_platform_warnings(kiarabot_${SERVICE}_application)
endforeach()

add_library(kiarabot_scheduler_runtime_client STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/scheduler_runtime_client.cpp)
target_include_directories(kiarabot_scheduler_runtime_client PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_scheduler_runtime_client PUBLIC
  kiarabot_scheduler_service kiarabot_proto kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_scheduler_runtime_client)

add_library(kiarabot_scheduler_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/scheduler_rpc.cpp)
target_include_directories(kiarabot_scheduler_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_scheduler_rpc PUBLIC kiarabot_scheduler_service kiarabot_proto
  kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_scheduler_rpc)

add_library(kiarabot_scheduler_service STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/scheduler_rules.cpp
  ${CMAKE_SOURCE_DIR}/core/src/application/target/scheduler_service.cpp)
target_include_directories(kiarabot_scheduler_service PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_scheduler_service PRIVATE OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_scheduler_service)
add_executable(kiarabot_scheduler_service_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/scheduler_service_test.cpp)
target_link_libraries(kiarabot_scheduler_service_test PRIVATE kiarabot_scheduler_service
  OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_scheduler_service_test)
add_test(NAME p10_scheduler_service COMMAND kiarabot_scheduler_service_test)
set_tests_properties(p10_scheduler_service PROPERTIES LABELS "p10;unit;fault" TIMEOUT 30)

add_library(kiarabot_control_application STATIC
  ${CMAKE_SOURCE_DIR}/control/api/src/application/target/application.cpp)
target_include_directories(kiarabot_control_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
target_link_libraries(kiarabot_control_application PUBLIC kiarabot_onboarding)
kiarabot_platform_warnings(kiarabot_control_application)

add_executable(kiarabot_service_applications_test
  ${CMAKE_SOURCE_DIR}/core/tests/architecture/service_applications_test.cpp)
target_link_libraries(kiarabot_service_applications_test PRIVATE
  kiarabot_identity_application kiarabot_runtime_application kiarabot_plugin_host_application
  kiarabot_scheduler_application kiarabot_model_application)
kiarabot_platform_warnings(kiarabot_service_applications_test)
add_test(NAME p02_service_applications COMMAND kiarabot_service_applications_test)
set_tests_properties(p02_service_applications PROPERTIES LABELS "p02;unit" TIMEOUT 10)

add_executable(kiarabot_control_application_target_test
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/application_test.cpp)
target_link_libraries(kiarabot_control_application_target_test PRIVATE kiarabot_control_application)
kiarabot_platform_warnings(kiarabot_control_application_target_test)
add_test(NAME p02_control_application COMMAND kiarabot_control_application_target_test)
set_tests_properties(p02_control_application PROPERTIES LABELS "p02;unit" TIMEOUT 10)

add_library(kiarabot_host_process STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/host_process.cpp)
target_include_directories(kiarabot_host_process PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_host_process)
if(UNIX)
  add_executable(kiarabot_host_process_test
    ${CMAKE_SOURCE_DIR}/core/tests/platform/host_process_test.cpp)
  target_link_libraries(kiarabot_host_process_test PRIVATE kiarabot_host_process)
  kiarabot_platform_warnings(kiarabot_host_process_test)
  add_test(NAME p08_host_process COMMAND kiarabot_host_process_test)
  set_tests_properties(p08_host_process PROPERTIES LABELS "p08;unit;fault" TIMEOUT 30)
endif()

add_executable(kiarabot_scheduler_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/scheduler_rpc_test.cpp)
target_link_libraries(kiarabot_scheduler_rpc_test PRIVATE kiarabot_scheduler_rpc)
kiarabot_platform_warnings(kiarabot_scheduler_rpc_test)
add_test(NAME p10_scheduler_rpc COMMAND kiarabot_scheduler_rpc_test)
set_tests_properties(p10_scheduler_rpc PROPERTIES LABELS "p10;integration;fault" TIMEOUT 60)

add_library(kiarabot_host_bridge STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/host_bridge_client.cpp)
target_include_directories(kiarabot_host_bridge PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_host_bridge PUBLIC kiarabot_proto)
kiarabot_platform_warnings(kiarabot_host_bridge)

if(UNIX)
  add_executable(kiarabot_host_bridge_test
    ${CMAKE_SOURCE_DIR}/core/tests/platform/host_bridge_test.cpp)
  target_link_libraries(kiarabot_host_bridge_test PRIVATE kiarabot_host_bridge)
  kiarabot_platform_warnings(kiarabot_host_bridge_test)
  add_test(NAME p08_host_bridge COMMAND kiarabot_host_bridge_test)
  set_tests_properties(p08_host_bridge PROPERTIES LABELS "p08;integration;fault" TIMEOUT 60)
endif()

add_library(kiarabot_host_service STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/host_service.cpp)
target_include_directories(kiarabot_host_service PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_host_service)
add_executable(kiarabot_host_service_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/host_service_test.cpp)
target_link_libraries(kiarabot_host_service_test PRIVATE kiarabot_host_service)
kiarabot_platform_warnings(kiarabot_host_service_test)
add_test(NAME p08_host_service COMMAND kiarabot_host_service_test)
set_tests_properties(p08_host_service PROPERTIES LABELS "p08;unit;fault" TIMEOUT 30)

add_library(kiarabot_service_config STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_config.cpp)
target_include_directories(kiarabot_service_config PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_service_config PRIVATE OpenSSL::SSL OpenSSL::Crypto kiarabot_core_json)
kiarabot_platform_warnings(kiarabot_service_config)
add_executable(kiarabot_service_config_driver
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_config_driver.cpp)
target_link_libraries(kiarabot_service_config_driver PRIVATE kiarabot_service_config)
kiarabot_platform_warnings(kiarabot_service_config_driver)
add_test(NAME p02_service_config COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_config_test.py
  $<TARGET_FILE:kiarabot_service_config_driver> $<TARGET_FILE:kiarabot-identity>)
set_tests_properties(p02_service_config PROPERTIES LABELS "p02;unit" TIMEOUT 30)

# Create one translation unit per new inner header: no generated/protocol/JSON includes provided.
file(GLOB_RECURSE INNER_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/domain/*.hpp
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/application/*.hpp
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/ports/*.hpp
  ${CMAKE_SOURCE_DIR}/control/api/include/kiarabot/control/application/target/*.hpp)
set(HEADER_UNITS)
foreach(HEADER IN LISTS INNER_HEADERS)
  string(SHA256 HEADER_ID "${HEADER}")
  set(HEADER_UNIT "${CMAKE_BINARY_DIR}/inner-headers/${HEADER_ID}.cpp")
  file(GENERATE OUTPUT "${HEADER_UNIT}" CONTENT "#include \"${HEADER}\"\n")
  list(APPEND HEADER_UNITS "${HEADER_UNIT}")
endforeach()
add_library(kiarabot_inner_headers OBJECT ${HEADER_UNITS})
target_include_directories(kiarabot_inner_headers PRIVATE
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
kiarabot_platform_warnings(kiarabot_inner_headers)

set(KIARABOT_TARGET_PYTHON "${CMAKE_SOURCE_DIR}/sdk/python/.venv/bin/python" CACHE FILEPATH
  "Locked SDK Python used for target generation and round-trip verification")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/python.stamp"
  COMMAND "${KIARABOT_TARGET_PYTHON}" "${CMAKE_SOURCE_DIR}/sdk/python/tools/generate_proto.py"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/generated/python.stamp"
  DEPENDS ${KIARABOT_PROTO_SOURCES} "${CMAKE_SOURCE_DIR}/sdk/python/tools/generate_proto.py"
  VERBATIM)
add_custom_target(kiarabot_python_proto_generate ALL
  DEPENDS "${CMAKE_BINARY_DIR}/generated/python.stamp")
add_test(NAME p01_target_roundtrip COMMAND ${CMAKE_COMMAND} -E env
  "PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated"
  "${KIARABOT_TARGET_PYTHON}" "${CMAKE_SOURCE_DIR}/proto/tests/target_contract_test.py"
  "${KIARABOT_DESCRIPTOR}" $<TARGET_FILE:kiarabot_target_roundtrip>)
# 311 spawned Python/C++ message round-trips plus descriptor/policy checks. Green runs
# measured 57.72-60.21s on the P00 macOS host, so the previous 60s limit failed under
# ordinary build/CI load. Keep headroom; this is a budget change, not a behavior change.
set_tests_properties(p01_target_roundtrip PROPERTIES LABELS "p01;integration" TIMEOUT 180)

add_library(kiarabot_service_process STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_process.cpp
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_rpc.cpp
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/peer_certificate.cpp)
target_include_directories(kiarabot_service_process PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_service_process PRIVATE
  kiarabot_service_config kiarabot_proto Threads::Threads)
kiarabot_platform_warnings(kiarabot_service_process)

# Identity wire adaptation stays outer and borrows the pure application through its port.
add_library(kiarabot_identity_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/identity_rpc.cpp)
target_include_directories(kiarabot_identity_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_identity_rpc PUBLIC kiarabot_proto PRIVATE
  kiarabot_target_codec kiarabot_service_process kiarabot_identity_admin_application
  kiarabot_identity_delegation_application kiarabot_identity_exchange_application OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_identity_rpc)
add_executable(kiarabot_identity_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_rpc_test.cpp)
target_link_libraries(kiarabot_identity_rpc_test PRIVATE
  kiarabot_identity_rpc kiarabot_identity_crypto)
kiarabot_platform_warnings(kiarabot_identity_rpc_test)
add_test(NAME p04_identity_rpc COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_rpc_test.py
  $<TARGET_FILE:kiarabot_identity_rpc_test>)
set_tests_properties(p04_identity_rpc PROPERTIES LABELS "p04;integration;security" TIMEOUT 60)

add_executable(kiarabot_peer_certificate_evidence_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/peer_certificate_test.cpp)
target_link_libraries(kiarabot_peer_certificate_evidence_test PRIVATE
  kiarabot_service_process kiarabot_proto)
kiarabot_platform_warnings(kiarabot_peer_certificate_evidence_test)
add_test(NAME p04_peer_certificate_evidence COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/peer_certificate_test.py
  $<TARGET_FILE:kiarabot_peer_certificate_evidence_test>)
set_tests_properties(p04_peer_certificate_evidence PROPERTIES
  LABELS "p04;integration;security" TIMEOUT 30)

add_executable(kiarabot_service_composition_driver
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_composition_driver.cpp)
target_link_libraries(kiarabot_service_composition_driver PRIVATE kiarabot_service_process kiarabot_proto)
kiarabot_platform_warnings(kiarabot_service_composition_driver)
add_executable(kiarabot_runtime_identity_client_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/runtime_identity_client_test.cpp)
target_link_libraries(kiarabot_runtime_identity_client_test PRIVATE
  kiarabot_runtime_identity_client nlohmann_json::nlohmann_json)
kiarabot_platform_warnings(kiarabot_runtime_identity_client_test)
add_test(NAME p05_runtime_identity_client COMMAND kiarabot_runtime_identity_client_test)
set_tests_properties(p05_runtime_identity_client PROPERTIES
  LABELS "p05;unit;security" TIMEOUT 30)

# Real two-process Runtime<->Identity authorization chain: owned PG cluster, offline bootstrap of
# the Runtime platform peer, real mTLS daemons and real delegation/CheckAuthorization decisions.
get_filename_component(RUNTIME_E2E_PYTHON_BIN_DIR "${KIARABOT_TARGET_PYTHON}" DIRECTORY)
get_filename_component(RUNTIME_E2E_OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
add_test(NAME p05_runtime_identity_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
  "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
  --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
  --initializer $<TARGET_FILE:kiarabot-pg-initialize>
  --test "${CMAKE_SOURCE_DIR}/core/tests/platform/runtime_identity_e2e_test.py"
  --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
  --test-argument "${KIARABOT_TEST_PG_BIN}"
  --test-argument $<TARGET_FILE:kiarabot-identity>
  --test-argument $<TARGET_FILE:kiarabot-runtime>)
set_tests_properties(p05_runtime_identity_e2e PROPERTIES
  LABELS "p05;p04;integration;security;fault" TIMEOUT 180
  ENVIRONMENT "PATH=${RUNTIME_E2E_OPENSSL_BIN_DIR}:${RUNTIME_E2E_PYTHON_BIN_DIR}:$ENV{PATH};PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated")

add_test(NAME p03_service_composition COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_composition_test.py
  $<TARGET_FILE:kiarabot_service_composition_driver> $<TARGET_FILE:kiarabot_service_rpc_client>)
set_tests_properties(p03_service_composition PROPERTIES LABELS "p03;integration;fault" TIMEOUT 30)

# P05 Runtime production unary transport. Only conversion and stable error mapping; all business
# rules stay in kiarabot_runtime_resource_application. Nine P06 internal_dispatcher methods are
# intentionally not overridden and keep generated UNIMPLEMENTED until P06 Bridge wiring.
# Runtime -> Identity owner client: online CheckAuthorization and delegation-metadata binding to the
# actual inbound TLS leaf. Production composition injects it; tests use a fake Identity server.
find_package(nlohmann_json 3.11.3 EXACT CONFIG REQUIRED)
add_library(kiarabot_model_identity_client STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_identity_client.cpp)
target_include_directories(kiarabot_model_identity_client PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_model_identity_client)
target_link_libraries(kiarabot_model_identity_client PUBLIC kiarabot_proto
  kiarabot_model_gateway_application
  PRIVATE kiarabot_target_codec kiarabot_service_process nlohmann_json::nlohmann_json)

add_library(kiarabot_runtime_identity_client STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/runtime_identity_client.cpp)
target_include_directories(kiarabot_runtime_identity_client PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_runtime_identity_client)
target_link_libraries(kiarabot_runtime_identity_client PUBLIC kiarabot_proto
  PRIVATE kiarabot_target_codec kiarabot_service_process nlohmann_json::nlohmann_json
          OpenSSL::Crypto)

add_library(kiarabot_runtime_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/runtime_rpc.cpp
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/runtime_bridge.cpp)
target_include_directories(kiarabot_runtime_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_runtime_rpc)
target_link_libraries(kiarabot_runtime_rpc PUBLIC kiarabot_proto kiarabot_target_codec
  PRIVATE kiarabot_runtime_resource_application OpenSSL::Crypto)

add_library(kiarabot_runtime_model_client STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/runtime_model_client.cpp)
target_include_directories(kiarabot_runtime_model_client PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_runtime_model_client)
target_link_libraries(kiarabot_runtime_model_client PUBLIC kiarabot_proto
  kiarabot_target_codec)

add_library(kiarabot_model_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_rpc.cpp)
target_include_directories(kiarabot_model_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_model_rpc)
target_link_libraries(kiarabot_model_rpc PUBLIC kiarabot_proto kiarabot_target_codec
  kiarabot_model_gateway_application)

add_executable(kiarabot_runtime_model_client_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/runtime_model_client_test.cpp)
target_link_libraries(kiarabot_runtime_model_client_test PRIVATE
  kiarabot_runtime_model_client)
kiarabot_platform_warnings(kiarabot_runtime_model_client_test)
add_test(NAME p11_runtime_model_client COMMAND kiarabot_runtime_model_client_test)
set_tests_properties(p11_runtime_model_client PROPERTIES LABELS "p11;integration" TIMEOUT 30)

add_executable(kiarabot_model_identity_client_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_identity_client_test.cpp)
target_link_libraries(kiarabot_model_identity_client_test PRIVATE
  kiarabot_model_identity_client)
kiarabot_platform_warnings(kiarabot_model_identity_client_test)
add_test(NAME p11_model_identity_client COMMAND kiarabot_model_identity_client_test)
set_tests_properties(p11_model_identity_client PROPERTIES LABELS "p11;unit;security" TIMEOUT 30)

# P11b integration additions use the real HTTP provider in the application fixture.
if(TARGET kiarabot_model_gateway_application_test)
  target_link_libraries(kiarabot_model_gateway_application_test PRIVATE
    kiarabot_model_http_provider)
endif()

add_executable(kiarabot_model_http_provider_test
  ${CMAKE_SOURCE_DIR}/infrastructure/tests/model_http_provider_test.cpp)
target_link_libraries(kiarabot_model_http_provider_test PRIVATE
  kiarabot_model_http_provider)
kiarabot_platform_warnings(kiarabot_model_http_provider_test)
add_test(NAME p11_model_http_provider COMMAND kiarabot_model_http_provider_test)
set_tests_properties(p11_model_http_provider PROPERTIES LABELS "p11;integration" TIMEOUT 60)

add_executable(kiarabot_model_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_rpc_test.cpp)
target_link_libraries(kiarabot_model_rpc_test PRIVATE
  kiarabot_model_rpc kiarabot_model_gateway_application kiarabot_service_process)
kiarabot_platform_warnings(kiarabot_model_rpc_test)
add_test(NAME p11_model_rpc COMMAND kiarabot_model_rpc_test)
set_tests_properties(p11_model_rpc PROPERTIES LABELS "p11;integration" TIMEOUT 30)

foreach(SERVICE identity runtime plugin_host scheduler model)
  set(ENTRY "${SERVICE}")
  set(BINARY "${SERVICE}")
  if(SERVICE STREQUAL "plugin_host")
    set(ENTRY host)
    set(BINARY plugin-host)
  endif()
  add_executable(kiarabot-${BINARY} ${CMAKE_SOURCE_DIR}/core/app/${ENTRY}_main.cpp)
  target_link_libraries(kiarabot-${BINARY} PRIVATE
    kiarabot_${SERVICE}_application kiarabot_service_process)
  if(SERVICE STREQUAL "plugin_host" AND TARGET kiarabot_host_journal AND
     TARGET kiarabot_host_service AND TARGET kiarabot_host_bridge)
    # Full builds compose the real Host service; dev builds keep the P02 readiness stub.
    target_sources(kiarabot-${BINARY} PRIVATE
      ${CMAKE_SOURCE_DIR}/core/app/host_composition.cpp)
    target_link_libraries(kiarabot-${BINARY} PRIVATE
      kiarabot_host_service kiarabot_host_bridge kiarabot_host_process kiarabot_host_journal
      OpenSSL::Crypto)
    target_compile_definitions(kiarabot-${BINARY} PRIVATE KIARABOT_HOST_COMPOSITION=1)
  endif()
  if(SERVICE STREQUAL "model")
    # P11a Model composition owns the typed PG factory, deployment root key, secret/digest
    # adapters and the development provider/RPC surface. HTTP provider and real Identity caller
    # verification are P11b/P11c.
    target_sources(kiarabot-model PRIVATE
      ${CMAKE_SOURCE_DIR}/core/app/model_composition.cpp)
    target_link_libraries(kiarabot-model PRIVATE
      kiarabot_model_rpc kiarabot_model_http_provider kiarabot_model_identity_client
      kiarabot_postgres kiarabot_identity_crypto OpenSSL::Crypto)
  endif()
  if(SERVICE STREQUAL "runtime")
    # The independent Runtime composition owns the typed PG factory, application and unary RPC
    # registration; keep the Identity/control composition out of this process.
    target_sources(kiarabot-runtime PRIVATE
      ${CMAKE_SOURCE_DIR}/core/app/runtime_composition.cpp)
    target_link_libraries(kiarabot-runtime PRIVATE
      kiarabot_runtime_model_client
      kiarabot_runtime_rpc kiarabot_runtime_identity_client kiarabot_postgres OpenSSL::Crypto)
  endif()
  set_target_properties(kiarabot-${BINARY} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  kiarabot_platform_warnings(kiarabot-${BINARY})
endforeach()
add_executable(kiarabot-control ${CMAKE_SOURCE_DIR}/control/api/app/main.cpp)
# control_auth supplies the service_process dependency after its own archive.
target_link_libraries(kiarabot-control PRIVATE kiarabot_control_application)
set_target_properties(kiarabot-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
kiarabot_platform_warnings(kiarabot-control)
add_executable(kiarabot_service_rpc_client ${CMAKE_SOURCE_DIR}/core/tests/platform/service_rpc_client.cpp)
target_link_libraries(kiarabot_service_rpc_client PRIVATE kiarabot_proto)
kiarabot_platform_warnings(kiarabot_service_rpc_client)

find_program(KIARABOT_TEST_OPENSSL openssl REQUIRED)
get_filename_component(OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
set(P02_BINARY_ARGUMENTS --control $<TARGET_FILE:kiarabot-control>
  --identity $<TARGET_FILE:kiarabot-identity> --runtime $<TARGET_FILE:kiarabot-runtime>
  --plugin-host $<TARGET_FILE:kiarabot-plugin-host> --scheduler $<TARGET_FILE:kiarabot-scheduler>
  --model $<TARGET_FILE:kiarabot-model>)
add_test(NAME p02_core_process COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_process_test.py
  $<TARGET_FILE:kiarabot-identity> $<TARGET_FILE:kiarabot-runtime> $<TARGET_FILE:kiarabot-plugin-host>
  $<TARGET_FILE:kiarabot-scheduler> $<TARGET_FILE:kiarabot-model> $<TARGET_FILE:kiarabot_service_rpc_client>)
set_tests_properties(p02_core_process PROPERTIES LABELS "p02;integration;fault" TIMEOUT 90)
foreach(GROUP lifecycle tls isolation)
  add_test(NAME architecture_p02_${GROUP} COMMAND "${KIARABOT_TARGET_PYTHON}"
    ${CMAKE_SOURCE_DIR}/tests/architecture/run.py --group ${GROUP}
    --openssl "${KIARABOT_TEST_OPENSSL}" ${P02_BINARY_ARGUMENTS})
  set_tests_properties(architecture_p02_${GROUP} PROPERTIES
    LABELS "p02;integration;fault" TIMEOUT 360)
endforeach()
foreach(FIXTURE fixture tls_fixture)
  add_test(NAME architecture_p02_${FIXTURE} COMMAND "${KIARABOT_TARGET_PYTHON}"
    ${CMAKE_SOURCE_DIR}/tests/architecture/${FIXTURE}_test.py --openssl "${KIARABOT_TEST_OPENSSL}")
  set(FIXTURE_LABELS "p02;unit")
  if(FIXTURE STREQUAL "tls_fixture")
    set(FIXTURE_LABELS "p02;integration")
  endif()
  set_tests_properties(architecture_p02_${FIXTURE} PROPERTIES LABELS "${FIXTURE_LABELS}" TIMEOUT 30)
endforeach()
add_test(NAME architecture_p02_oracle COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/support_test.py)
set_tests_properties(architecture_p02_oracle PROPERTIES LABELS "p02;unit" TIMEOUT 30)
add_test(NAME architecture_p02_references COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/check_references.py)
set_tests_properties(architecture_p02_references PROPERTIES LABELS "p02;unit;reference" TIMEOUT 10)
set_tests_properties(p02_service_config p02_core_process p03_service_composition p04_peer_certificate_evidence p04_identity_rpc PROPERTIES
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
add_test(NAME p01_target_api COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/quality/check-target-api.sh)
set_tests_properties(p01_target_api PROPERTIES LABELS "p01;unit" TIMEOUT 30)

add_test(NAME architecture_p02_reference_manifest_test COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/reference_test.py)
set_tests_properties(architecture_p02_reference_manifest_test PROPERTIES LABELS "p02;unit" TIMEOUT 10)
