# New Core targets use only core/include; old protocol/snapshot code is reference-only.
add_subdirectory(reference "${CMAKE_BINARY_DIR}/reference/core")

# P01 pure internal-model assertions are not reference business tests.
foreach(COMPONENT foundation invariants)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME architecture_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(architecture_${COMPONENT} PROPERTIES LABELS "p01;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT runtime_event_delivery)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p06_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p06_${COMPONENT} PROPERTIES LABELS "p06;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT action_rules)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p07_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p07_${COMPONENT} PROPERTIES LABELS "p07;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT model_gateway)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>)
  add_test(NAME p11_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p11_${COMPONENT} PROPERTIES LABELS "p11;unit" TIMEOUT 10)
endforeach()

# P11 Model gateway application stays pure: injected provider/crypto/authorization ports and a
# typed Model UoW; no SQL, protobuf, HTTP or logging dependency.
add_library(kiarabot_model_gateway_application STATIC
  src/application/target/model_gateway_application.cpp)
set_property(TARGET kiarabot_model_gateway_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_model_gateway_application PUBLIC include)
target_compile_features(kiarabot_model_gateway_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_model_gateway_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_model_gateway_application_test
  tests/architecture/model_gateway_application_test.cpp)
target_link_libraries(kiarabot_model_gateway_application_test PRIVATE
  kiarabot_model_gateway_application)
# The protocol-enabled composition later links the real HTTP and identity-crypto adapters.
# Keep the injected-port application assertions available in the dependency-light dev preset.
if(KIARABOT_GENERATE_PROTO)
  target_compile_definitions(kiarabot_model_gateway_application_test PRIVATE
    KIARABOT_TEST_MODEL_INFRASTRUCTURE=1)
endif()
target_compile_options(kiarabot_model_gateway_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p11_model_gateway_application
  COMMAND kiarabot_model_gateway_application_test)
set_tests_properties(p11_model_gateway_application PROPERTIES
  LABELS "p11;unit" TIMEOUT 30)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/model-streaming-application.cmake)

foreach(COMPONENT runtime_host_application)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p08_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p08_${COMPONENT} PROPERTIES LABELS "p08;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT persistence_contract persistence_rules time)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p03_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p03_${COMPONENT} PROPERTIES LABELS "p03;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT identity_contract identity_application_contract identity_verification_keys_contract
    session_control_peer_contract identity_assignment_repository_contract
    identity_assignment_fingerprint_contract identity_registration_snapshot identity_assignment_snapshot)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p04_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p04_${COMPONENT} PROPERTIES LABELS "p04;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT identity_rules identity_grant_projection identity_assignment_state)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p04_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p04_${COMPONENT} PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)
endforeach()

# Identity administrative/delegation use cases stay pure and independently linkable.
foreach(COMPONENT identity_admin identity_delegation identity_exchange)
  add_library(kiarabot_${COMPONENT}_application STATIC
    src/application/target/${COMPONENT}_application.cpp)
  set_property(TARGET kiarabot_${COMPONENT}_application PROPERTY KIARABOT_LAYER INNER)
  target_include_directories(kiarabot_${COMPONENT}_application PUBLIC include)
  target_compile_features(kiarabot_${COMPONENT}_application PUBLIC cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_application PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
endforeach()
add_executable(kiarabot_identity_exchange_application_test
  tests/architecture/identity_exchange_application_test.cpp)
target_link_libraries(kiarabot_identity_exchange_application_test PRIVATE
  kiarabot_identity_exchange_application)
target_compile_options(kiarabot_identity_exchange_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_exchange_application COMMAND kiarabot_identity_exchange_application_test)
set_tests_properties(p04_identity_exchange_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)
add_executable(kiarabot_identity_admin_application_test
  tests/architecture/identity_admin_application_test.cpp)
target_link_libraries(kiarabot_identity_admin_application_test PRIVATE
  kiarabot_identity_admin_application kiarabot_identity_delegation_application)
target_compile_options(kiarabot_identity_admin_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_test(NAME p04_identity_admin_application COMMAND kiarabot_identity_admin_application_test)
set_tests_properties(p04_identity_admin_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)

add_library(kiarabot_identity_security_application STATIC
  src/application/target/identity_security_application.cpp)
set_property(TARGET kiarabot_identity_security_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_security_application PUBLIC include)
target_compile_features(kiarabot_identity_security_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_security_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_identity_security_application_test
  tests/architecture/identity_security_application_test.cpp)
target_link_libraries(kiarabot_identity_security_application_test PRIVATE
  kiarabot_identity_security_application)
target_compile_options(kiarabot_identity_security_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_test(NAME p04_identity_security_application COMMAND kiarabot_identity_security_application_test)
set_tests_properties(p04_identity_security_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 30)

# Five principal/credential management use cases stay pure and independently linkable. The
# application owns no SQL/protobuf type; the composition root injects the fingerprinting port
# adapter and the outer PG security factory.
add_library(kiarabot_identity_principal_application STATIC
  src/application/target/identity_principal_application.cpp)
set_property(TARGET kiarabot_identity_principal_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_principal_application PUBLIC include)
target_compile_features(kiarabot_identity_principal_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_principal_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_identity_principal_application_test
  tests/architecture/identity_principal_application_test.cpp)
target_link_libraries(kiarabot_identity_principal_application_test PRIVATE
  kiarabot_identity_principal_application)
target_compile_options(kiarabot_identity_principal_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_principal_application
  COMMAND kiarabot_identity_principal_application_test)
set_tests_properties(p04_identity_principal_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 30)

find_program(KIARABOT_TEST_RUBY ruby REQUIRED)
add_test(NAME p04_identity_role_catalog COMMAND "${KIARABOT_TEST_RUBY}"
  ${CMAKE_SOURCE_DIR}/proto/tests/identity_role_catalog_test.rb
  $<TARGET_FILE:kiarabot_identity_rules_test> ${CMAKE_SOURCE_DIR}/api/openapi.yaml)
set_tests_properties(p04_identity_role_catalog PROPERTIES
  LABELS "p04;unit;security;contract" TIMEOUT 10)

# Public-key lookup is a pure use case; the shared security helper is an internal header.
add_library(kiarabot_identity_verification_keys_application STATIC
  src/application/target/identity_verification_keys_application.cpp)
set_property(TARGET kiarabot_identity_verification_keys_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_verification_keys_application PUBLIC include)
target_compile_features(kiarabot_identity_verification_keys_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_verification_keys_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_identity_verification_keys_application_test
  tests/architecture/identity_verification_keys_application_test.cpp)
target_link_libraries(kiarabot_identity_verification_keys_application_test PRIVATE
  kiarabot_identity_verification_keys_application)
target_compile_options(kiarabot_identity_verification_keys_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_verification_keys_application
  COMMAND kiarabot_identity_verification_keys_application_test)
set_tests_properties(p04_identity_verification_keys_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)

# Online authority checking remains independent of proto, RPC, crypto implementations and SQL.
add_library(kiarabot_identity_authorization_application STATIC
  src/application/target/identity_authorization_application.cpp)
set_property(TARGET kiarabot_identity_authorization_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_authorization_application PUBLIC include)
target_compile_features(kiarabot_identity_authorization_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_authorization_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_identity_authorization_application_test
  tests/architecture/identity_authorization_application_test.cpp)
target_link_libraries(kiarabot_identity_authorization_application_test PRIVATE
  kiarabot_identity_authorization_application)
target_compile_options(kiarabot_identity_authorization_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_authorization_application COMMAND kiarabot_identity_authorization_application_test)
set_tests_properties(p04_identity_authorization_application PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)

# Durable bounded invalidation stream/snapshot use cases: read-only online authority, no cached
# subscription grants. Production PostgreSQL supplies the log; a missing store fails closed.
add_executable(kiarabot_p12e_audit_query_test tests/architecture/p12e_audit_query_test.cpp)
target_include_directories(kiarabot_p12e_audit_query_test PRIVATE include)
target_compile_features(kiarabot_p12e_audit_query_test PRIVATE cxx_std_20)
target_compile_options(kiarabot_p12e_audit_query_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p12e_audit_query COMMAND kiarabot_p12e_audit_query_test)
set_tests_properties(p12e_audit_query PROPERTIES LABELS "p12;unit" TIMEOUT 10)

# P12e: read-only Identity/Runtime audit-log aggregation projections. Pure applications; the
# transport/composition owns RPC wiring and the real PG source remains owner-local.
add_library(kiarabot_identity_audit_application STATIC
  src/application/target/identity_audit_application.cpp)
set_property(TARGET kiarabot_identity_audit_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_audit_application PUBLIC include)
target_compile_features(kiarabot_identity_audit_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_audit_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_library(kiarabot_runtime_audit_application STATIC
  src/application/target/runtime_audit_application.cpp)
set_property(TARGET kiarabot_runtime_audit_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_runtime_audit_application PUBLIC include)
target_compile_features(kiarabot_runtime_audit_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_runtime_audit_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)

# P12l read-only human principal directory; pure application, no proto/SQL in the library.
add_library(kiarabot_identity_principal_directory_application STATIC
  src/application/target/identity_principal_directory_application.cpp)
set_property(TARGET kiarabot_identity_principal_directory_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_principal_directory_application PUBLIC include)
target_compile_features(kiarabot_identity_principal_directory_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_principal_directory_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_library(kiarabot_identity_invalidation_application STATIC
  src/application/target/identity_invalidation_application.cpp)
set_property(TARGET kiarabot_identity_invalidation_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_invalidation_application PUBLIC include)
target_compile_features(kiarabot_identity_invalidation_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_invalidation_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)

# Assignment management stays independent of proto, RPC, crypto implementations and SQL; typed
# fingerprints are an injected port and every write owns ONE short security transaction.
add_library(kiarabot_identity_assignment_application STATIC
  src/application/target/identity_assignment_application.cpp)
set_property(TARGET kiarabot_identity_assignment_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_assignment_application PUBLIC include)
target_compile_features(kiarabot_identity_assignment_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_assignment_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
target_link_libraries(kiarabot_identity_assignment_application PRIVATE
  kiarabot_identity_invalidation_application)
add_executable(kiarabot_identity_assignment_application_test
  tests/architecture/identity_assignment_application_test.cpp)
target_link_libraries(kiarabot_identity_assignment_application_test PRIVATE
  kiarabot_identity_assignment_application)
target_compile_options(kiarabot_identity_assignment_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_assignment_application COMMAND kiarabot_identity_assignment_application_test)
set_tests_properties(p04_identity_assignment_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)

target_link_libraries(kiarabot_identity_principal_application PRIVATE
  kiarabot_identity_invalidation_application)
add_executable(kiarabot_identity_invalidation_application_test
  tests/architecture/identity_invalidation_application_test.cpp)
target_link_libraries(kiarabot_identity_invalidation_application_test PRIVATE
  kiarabot_identity_invalidation_application)
target_compile_options(kiarabot_identity_invalidation_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p04_identity_invalidation_application
  COMMAND kiarabot_identity_invalidation_application_test)
set_tests_properties(p04_identity_invalidation_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)
# P05 Runtime resources, bindings and current generations. The application owns business rules over
# the pure Runtime UoW ports; RPC/transport mapping and PG adapters stay in outer layers.
add_library(kiarabot_runtime_resource_application STATIC
  src/application/target/runtime_resource_application.cpp
  src/application/target/runtime_resources/account_adapter.cpp
  src/application/target/runtime_resources/common.cpp
  src/application/target/runtime_resources/host_command_snapshot.cpp
  src/application/target/runtime_resources/host_commands.cpp
  src/application/target/runtime_resources/instances.cpp
  src/application/target/runtime_resources/lifecycle.cpp
  src/application/target/runtime_resources/timer.cpp
  src/application/target/runtime_bridge_application.cpp
  src/application/target/runtime_action_application.cpp
  src/application/target/runtime_task_application.cpp
  src/application/target/runtime_plugin_capability.cpp
  src/application/target/runtime_lifecycle_participants.cpp)
set_property(TARGET kiarabot_runtime_resource_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_runtime_resource_application PUBLIC include)
target_compile_features(kiarabot_runtime_resource_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_runtime_resource_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_runtime_rules_test tests/architecture/runtime_rules_test.cpp)
target_include_directories(kiarabot_runtime_rules_test PRIVATE include)
target_compile_features(kiarabot_runtime_rules_test PRIVATE cxx_std_20)
target_compile_options(kiarabot_runtime_rules_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p05_runtime_rules COMMAND kiarabot_runtime_rules_test)
set_tests_properties(p05_runtime_rules PROPERTIES LABELS "p05;unit" TIMEOUT 10)
add_executable(kiarabot_runtime_resource_application_test
  tests/architecture/runtime_resource_application_test.cpp)
target_link_libraries(kiarabot_runtime_resource_application_test PRIVATE
  kiarabot_runtime_resource_application)
target_compile_options(kiarabot_runtime_resource_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p05_runtime_resource_application
  COMMAND kiarabot_runtime_resource_application_test)
set_tests_properties(p05_runtime_resource_application PROPERTIES
  LABELS "p05;unit" TIMEOUT 30)

# P13a/P13b lifecycle drain/recovery application matrix label; the same binary also carries the
# historical p05 label so existing phase gates keep their registration.
add_test(NAME p13_runtime_lifecycle COMMAND kiarabot_runtime_resource_application_test)
set_tests_properties(p13_runtime_lifecycle PROPERTIES
  LABELS "p13;unit;fault" TIMEOUT 30)

# P13h-2 production Scheduler lifecycle participant application matrix (fake Scheduler port).
add_executable(kiarabot_runtime_scheduler_lifecycle_test
  tests/architecture/runtime_scheduler_lifecycle_participant_test.cpp)
target_link_libraries(kiarabot_runtime_scheduler_lifecycle_test PRIVATE
  kiarabot_runtime_resource_application)
target_compile_options(kiarabot_runtime_scheduler_lifecycle_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p13_scheduler_lifecycle
  COMMAND kiarabot_runtime_scheduler_lifecycle_test)
set_tests_properties(p13_scheduler_lifecycle PROPERTIES
  LABELS "p13;unit;fault" TIMEOUT 30)

# P09a Runtime storage/shared-namespace domain+application (phase 1: no PG; ports fail closed
# until the P07a-A-based PG phase lands). Pure application owns authorization, CAS/condition,
# quota, receipt replay and audit ordering.
add_library(kiarabot_runtime_storage_application STATIC
  src/application/target/runtime_storage_application.cpp)
set_property(TARGET kiarabot_runtime_storage_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_runtime_storage_application PUBLIC include)
target_compile_features(kiarabot_runtime_storage_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_runtime_storage_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_runtime_storage_application_test
  tests/architecture/runtime_storage_application_test.cpp)
target_include_directories(kiarabot_runtime_storage_application_test PRIVATE include)
target_link_libraries(kiarabot_runtime_storage_application_test PRIVATE
  kiarabot_runtime_storage_application)
target_compile_features(kiarabot_runtime_storage_application_test PRIVATE cxx_std_20)
target_compile_options(kiarabot_runtime_storage_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p09a_runtime_storage_application
  COMMAND kiarabot_runtime_storage_application_test)
set_tests_properties(p09a_runtime_storage_application PROPERTIES
  LABELS "p09;unit" TIMEOUT 30)

add_executable(kiarabot_runtime_plugin_capability_test
  tests/architecture/runtime_plugin_capability_test.cpp)
target_link_libraries(kiarabot_runtime_plugin_capability_test PRIVATE
  kiarabot_runtime_resource_application)
target_compile_options(kiarabot_runtime_plugin_capability_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p06_runtime_plugin_capability COMMAND kiarabot_runtime_plugin_capability_test)
set_tests_properties(p06_runtime_plugin_capability PROPERTIES LABELS "p06;unit;security" TIMEOUT 10)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/operation-observation.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/host-maintenance.cmake)
