# PostgreSQL is an outer adapter; no database dependency reaches application libraries.
find_package(libpqxx 7.10.0 EXACT CONFIG REQUIRED)
add_library(kiarabot_postgres STATIC
  src/database.cpp src/common_work.cpp src/record_codec.cpp src/factories.cpp
  src/identity_security.cpp src/identity_sessions.cpp src/identity_receipts.cpp
  src/identity_assignments.cpp src/identity_invalidations.cpp src/runtime_events.cpp
  src/model_store.cpp src/action_records.cpp src/storage_records.cpp
  src/host_registration.cpp src/host_command_outbox.cpp src/runtime_management_receipts.cpp
  src/operation_observations.cpp src/bootstrap_delivery_records.cpp)
target_include_directories(kiarabot_postgres PUBLIC
  include ${CMAKE_SOURCE_DIR}/core/include)
# Keep the native libpq target and its transitive TLS libraries in static link order.
target_link_libraries(kiarabot_postgres PRIVATE libpqxx::pqxx PostgreSQL::PostgreSQL Threads::Threads)
kiarabot_platform_warnings(kiarabot_postgres)

# These composition roots exist only in the root proto/full build, like this adapter.
foreach(SERVICE identity runtime scheduler model)
  target_link_libraries(kiarabot-${SERVICE} PRIVATE kiarabot_postgres)
endforeach()

add_executable(kiarabot-pg-initialize migrate/initialize_main.cpp)
target_link_libraries(kiarabot-pg-initialize PRIVATE kiarabot_postgres)
kiarabot_platform_warnings(kiarabot-pg-initialize)

if(UNIX)
  add_executable(kiarabot_postgres_operation_observations_test
    tests/operation_observations_test.cpp)
  target_link_libraries(kiarabot_postgres_operation_observations_test PRIVATE
    kiarabot_postgres libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_operation_observations_test)
  add_test(NAME ws02_postgres_operation_observations COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_operation_observations_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(ws02_postgres_operation_observations PROPERTIES
    LABELS "ws02;integration;fault" TIMEOUT 180)
  # BOOT-02 real PG evidence: delivery intent/receipt CAS, version reissue and Control report
  # idempotency/conflict on an owned PostgreSQL 16.4 fixture.
  add_executable(kiarabot_postgres_bootstrap_delivery_test
    tests/bootstrap_delivery_test.cpp)
  target_link_libraries(kiarabot_postgres_bootstrap_delivery_test PRIVATE
    kiarabot_postgres kiarabot_runtime_resource_application OpenSSL::Crypto libpqxx::pqxx)
  target_include_directories(kiarabot_postgres_bootstrap_delivery_test SYSTEM PRIVATE
    $<TARGET_PROPERTY:OpenSSL::Crypto,INTERFACE_INCLUDE_DIRECTORIES>)
  kiarabot_platform_warnings(kiarabot_postgres_bootstrap_delivery_test)
  add_test(NAME p05_postgres_bootstrap_delivery COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_bootstrap_delivery_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p05_postgres_bootstrap_delivery PROPERTIES
    LABELS "p05;boot;integration;fault" TIMEOUT 180)
  add_executable(kiarabot_identity_application_test
    tests/identity_application_test.cpp tests/identity_application_tls.cpp
    tests/identity_authorization_checks.cpp)
  target_link_libraries(kiarabot_identity_application_test PRIVATE
    kiarabot_postgres kiarabot_identity_crypto kiarabot_identity_admin_application
    kiarabot_identity_delegation_application kiarabot_identity_security_application
    kiarabot_identity_principal_application kiarabot_identity_authorization_application
    kiarabot_identity_invalidation_application kiarabot_identity_principal_directory_application
    libpqxx::pqxx)
  # Static PG/crypto adapters already supply SSL/Crypto linkage; these headers are used directly.
  target_include_directories(kiarabot_identity_application_test SYSTEM PRIVATE
    $<TARGET_PROPERTY:OpenSSL::Crypto,INTERFACE_INCLUDE_DIRECTORIES>)
  kiarabot_platform_warnings(kiarabot_identity_application_test)
  add_test(NAME p04_identity_application COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_identity_application_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p04_identity_application PROPERTIES
    LABELS "p04;integration;security;fault" TIMEOUT 120)

  add_executable(kiarabot_postgres_runtime_events_test tests/runtime_events_test.cpp)
  target_link_libraries(kiarabot_postgres_runtime_events_test PRIVATE
    kiarabot_postgres kiarabot_runtime_resource_application kiarabot_identity_crypto libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_runtime_events_test)
  add_executable(kiarabot_postgres_runtime_bridge_seed tests/runtime_bridge_seed.cpp)
  target_link_libraries(kiarabot_postgres_runtime_bridge_seed PRIVATE
    kiarabot_postgres libpqxx::pqxx OpenSSL::Crypto)
  kiarabot_platform_warnings(kiarabot_postgres_runtime_bridge_seed)
  add_test(NAME p06_client_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/tests/architecture/p06_client_e2e.py"
    --runtime $<TARGET_FILE:kiarabot-runtime>
    --seed $<TARGET_FILE:kiarabot_postgres_runtime_bridge_seed>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --openssl "${KIARABOT_TEST_OPENSSL}"
    --pg-bin "${KIARABOT_TEST_PG_BIN}"
    --repo-source "${CMAKE_SOURCE_DIR}")
  set_tests_properties(p06_client_e2e PROPERTIES
    LABELS "p06;integration;fault" TIMEOUT 240)

  # P07d: real NoneBot adapter process + fake loopback OneBot WS + real Runtime bridge. The
  # adapter test environment (nonebot2[websockets]) is installed by CI into adapters/nonebot/.venv;
  # the script drives the real adapter child with that interpreter while the Runtime and seed are
  # the same production binaries as p06_client_e2e.
  if(EXISTS "${CMAKE_SOURCE_DIR}/adapters/nonebot/.venv/bin/python")
    add_test(NAME p07_adapter_e2e COMMAND
      "${CMAKE_SOURCE_DIR}/adapters/nonebot/.venv/bin/python"
      "${CMAKE_SOURCE_DIR}/tests/architecture/p07_adapter_e2e.py"
      --runtime $<TARGET_FILE:kiarabot-runtime>
      --seed $<TARGET_FILE:kiarabot_postgres_runtime_bridge_seed>
      --initializer $<TARGET_FILE:kiarabot-pg-initialize>
      --openssl "${KIARABOT_TEST_OPENSSL}"
      --pg-bin "${KIARABOT_TEST_PG_BIN}"
      --repo-source "${CMAKE_SOURCE_DIR}")
    set_tests_properties(p07_adapter_e2e PROPERTIES
      LABELS "p06;p07;integration;fault" TIMEOUT 300)
  endif()
  add_test(NAME p08_host_bridge_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/tests/architecture/p08_host_bridge_e2e.py"
    --runtime $<TARGET_FILE:kiarabot-runtime>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --openssl "${KIARABOT_TEST_OPENSSL}"
    --pg-bin "${KIARABOT_TEST_PG_BIN}"
    --repo-source "${CMAKE_SOURCE_DIR}")
  set_tests_properties(p08_host_bridge_e2e PROPERTIES
    LABELS "p08;integration;fault" TIMEOUT 240)
  # P13g test-only joint delete/restore e2e: real PG16.4 + real mTLS Runtime (test composition
  # binary) + fake HostBridge client + injected fake Scheduler participant. Not production evidence.
  if(TARGET kiarabot-runtime-p13g)
    add_test(NAME p13_joint_delete_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
      "${CMAKE_SOURCE_DIR}/tests/architecture/p13_joint_delete_e2e.py"
      --runtime $<TARGET_FILE:kiarabot-runtime-p13g>
      --initializer $<TARGET_FILE:kiarabot-pg-initialize>
      --openssl "${KIARABOT_TEST_OPENSSL}"
      --pg-bin "${KIARABOT_TEST_PG_BIN}"
      --repo-source "${CMAKE_SOURCE_DIR}")
    set_tests_properties(p13_joint_delete_e2e PROPERTIES
      LABELS "p13;test-only;integration;fault" TIMEOUT 240)
  endif()
  add_test(NAME p08_managed_host_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/tests/architecture/p08_managed_host_e2e.py"
    --runtime $<TARGET_FILE:kiarabot-runtime>
    --host-binary $<TARGET_FILE:kiarabot-plugin-host>
    --seed $<TARGET_FILE:kiarabot_postgres_runtime_bridge_seed>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --openssl "${KIARABOT_TEST_OPENSSL}"
    --pg-bin "${KIARABOT_TEST_PG_BIN}"
    --repo-source "${CMAKE_SOURCE_DIR}")
  set_tests_properties(p08_managed_host_e2e PROPERTIES
    LABELS "p08;p13;integration;fault" TIMEOUT 300)
  # P13h-3 production joint lifecycle e2e: owned fixture driver supplies real PG16.4/PKI/
  # bootstrap; real Identity/Runtime/Scheduler + real plugin-host/managed SDK plugin.
  add_test(NAME p13h3_joint_delete_production_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/tests/architecture/p13h3_joint_delete_production_e2e.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-runtime>
    --test-argument $<TARGET_FILE:kiarabot-scheduler>
    --test-argument $<TARGET_FILE:kiarabot_postgres_runtime_bridge_seed>
    --test-argument $<TARGET_FILE:kiarabot-plugin-host>)
  set_tests_properties(p13h3_joint_delete_production_e2e PROPERTIES
    LABELS "p13;p13h3;p08;p10;integration;security;fault" TIMEOUT 900
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PATH=${RUNTIME_E2E_OPENSSL_BIN_DIR}:${RUNTIME_E2E_PYTHON_BIN_DIR}:$ENV{PATH};PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated")
  add_test(NAME p06_bridge_acceptance COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/tests/architecture/p06_bridge_acceptance.py"
    --runtime $<TARGET_FILE:kiarabot-runtime>
    --seed $<TARGET_FILE:kiarabot_postgres_runtime_bridge_seed>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --openssl "${KIARABOT_TEST_OPENSSL}"
    --pg-bin "${KIARABOT_TEST_PG_BIN}"
    --repo-source "${CMAKE_SOURCE_DIR}")
  set_tests_properties(p06_bridge_acceptance PROPERTIES
    LABELS "p06;p07;integration;fault" TIMEOUT 300)
  add_test(NAME p06_postgres_runtime_events COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_runtime_events_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p06_postgres_runtime_events PROPERTIES
    LABELS "p06;integration;fault" TIMEOUT 180)

  # P14f: native single-host target stack smoke (no Docker). Real native service binaries,
  # owned PG16.4 fixture, temporary PKI and offline identity bootstrap; this is an acceptance
  # harness, not the production Compose deployment.
  get_filename_component(P14F_PYTHON_BIN_DIR "${KIARABOT_TARGET_PYTHON}" DIRECTORY)
  get_filename_component(P14F_OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
  add_test(NAME p14_native_single_host COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/tests/architecture/p14_native_single_host.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-runtime>
    --test-argument $<TARGET_FILE:kiarabot-scheduler>
    --test-argument $<TARGET_FILE:kiarabot-model>
    --test-argument $<TARGET_FILE:kiarabot-control>)
  set_tests_properties(p14_native_single_host PROPERTIES
    LABELS "p14;integration;fault" TIMEOUT 300
    ENVIRONMENT "PATH=${P14F_OPENSSL_BIN_DIR}:${P14F_PYTHON_BIN_DIR}:$ENV{PATH}")

  # P12j: Panel API client contract matrix against the same real native target stack.
  # Default mode records the pre-P14g/P14h fail-closed 400s as known and still asserts all
  # auth/error/page shapes; after the fixes are merged rerun with KIARABOT_P12J_EXPECT_FIXED=1
  # (see reports/modules/architecture-p12j-panel-live.md) to require owner-backed success.
  add_test(NAME p12j_panel_live COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/tests/architecture/p14_native_single_host.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-runtime>
    --test-argument $<TARGET_FILE:kiarabot-scheduler>
    --test-argument $<TARGET_FILE:kiarabot-model>
    --test-argument $<TARGET_FILE:kiarabot-control>)
  set_tests_properties(p12j_panel_live PROPERTIES
    LABELS "p12;p14;integration;fault" TIMEOUT 300
    ENVIRONMENT "PATH=${P14F_OPENSSL_BIN_DIR}:${P14F_PYTHON_BIN_DIR}:$ENV{PATH};KIARABOT_P12J_PANEL=1")

  add_executable(kiarabot_postgres_storage_records_test tests/storage_records_test.cpp)
  target_link_libraries(kiarabot_postgres_storage_records_test PRIVATE
    kiarabot_postgres kiarabot_runtime_storage_application libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_storage_records_test)
  add_test(NAME p09_postgres_storage_records COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_storage_records_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p09_postgres_storage_records PROPERTIES
    LABELS "p09;integration;fault" TIMEOUT 180)

  add_executable(kiarabot_postgres_action_records_test tests/action_records_test.cpp)
  target_link_libraries(kiarabot_postgres_action_records_test PRIVATE
    kiarabot_postgres libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_action_records_test)
  add_test(NAME p07_postgres_action_records COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_action_records_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p07_postgres_action_records PROPERTIES
    LABELS "p07;integration;fault" TIMEOUT 180)

  add_executable(kiarabot_postgres_scheduler_test
    tests/scheduler_application_test.cpp)
  target_link_libraries(kiarabot_postgres_scheduler_test PRIVATE
    kiarabot_postgres kiarabot_scheduler_service libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_scheduler_test)
  add_test(NAME p10_postgres_scheduler COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_scheduler_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p10_postgres_scheduler PROPERTIES
    LABELS "p10;integration;fault" TIMEOUT 180)

  add_executable(kiarabot_postgres_host_registration_test
    tests/host_registration_test.cpp)
  target_link_libraries(kiarabot_postgres_host_registration_test PRIVATE
    kiarabot_postgres libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_host_registration_test)
  add_test(NAME p08_postgres_host_registration COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_host_registration_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p08_postgres_host_registration PROPERTIES
    LABELS "p08;integration;fault" TIMEOUT 120)

  add_executable(kiarabot_postgres_database_test
    tests/database_test.cpp tests/common_work_checks.cpp tests/typed_work_checks.cpp
    tests/runtime_management_receipt_checks.cpp)
  target_link_libraries(kiarabot_postgres_database_test PRIVATE
    kiarabot_postgres libpqxx::pqxx kiarabot_runtime_resource_application)
  kiarabot_platform_warnings(kiarabot_postgres_database_test)
  # One process owns setup, faults and cleanup; parallel runs have private clusters.
  add_test(NAME p03_postgres_database COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_database_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p03_postgres_database PROPERTIES LABELS "p03;integration;fault" TIMEOUT 120)

  get_filename_component(P03_PYTHON_BIN_DIR "${KIARABOT_TARGET_PYTHON}" DIRECTORY)
  get_filename_component(P03_OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
  add_test(NAME p03_service_database COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test "${CMAKE_SOURCE_DIR}/core/tests/platform/service_database_test.py"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p03_service_database PROPERTIES LABELS "p03;integration;fault" TIMEOUT 120
    ENVIRONMENT "KIARABOT_TEST_PG_BIN=${KIARABOT_TEST_PG_BIN};KIARABOT_SERVICE_BIN_DIR=$<TARGET_FILE_DIR:kiarabot-identity>;PATH=${P03_PYTHON_BIN_DIR}:${P03_OPENSSL_BIN_DIR}:$ENV{PATH}")
endif()

# P10a Scheduler composition root: real PG factory + IANA resolver + unary RPC + maintenance.
if(TARGET kiarabot-scheduler AND TARGET kiarabot_scheduler_rpc)
  target_sources(kiarabot-scheduler PRIVATE
    ${CMAKE_SOURCE_DIR}/core/app/scheduler_composition.cpp)
  target_link_libraries(kiarabot-scheduler PRIVATE
    kiarabot_scheduler_rpc kiarabot_scheduler_runtime_client kiarabot_scheduler_identity_client
    kiarabot_scheduler_service kiarabot_postgres OpenSSL::Crypto)
endif()
