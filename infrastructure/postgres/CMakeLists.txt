# PostgreSQL is an outer adapter; no database dependency reaches application libraries.
find_package(libpqxx 7.10.0 EXACT CONFIG REQUIRED)
add_library(kiarabot_postgres STATIC
  src/database.cpp src/common_work.cpp src/record_codec.cpp src/factories.cpp
  src/identity_security.cpp src/identity_sessions.cpp src/identity_receipts.cpp
  src/identity_assignments.cpp src/identity_invalidations.cpp src/runtime_events.cpp
  src/model_store.cpp src/action_records.cpp src/storage_records.cpp
  src/host_registration.cpp src/host_command_outbox.cpp)
target_include_directories(kiarabot_postgres PUBLIC
  include ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_postgres PRIVATE libpqxx::pqxx Threads::Threads)
kiarabot_platform_warnings(kiarabot_postgres)

# These composition roots exist only in the root proto/full build, like this adapter.
foreach(SERVICE identity runtime scheduler model)
  target_link_libraries(kiarabot-${SERVICE} PRIVATE kiarabot_postgres)
endforeach()

add_executable(kiarabot-pg-initialize migrate/initialize_main.cpp)
target_link_libraries(kiarabot-pg-initialize PRIVATE kiarabot_postgres)
kiarabot_platform_warnings(kiarabot-pg-initialize)

if(UNIX)
  add_executable(kiarabot_identity_application_test
    tests/identity_application_test.cpp tests/identity_application_tls.cpp
    tests/identity_authorization_checks.cpp)
  target_link_libraries(kiarabot_identity_application_test PRIVATE
    kiarabot_postgres kiarabot_identity_crypto kiarabot_identity_admin_application
    kiarabot_identity_delegation_application kiarabot_identity_security_application
    kiarabot_identity_principal_application kiarabot_identity_authorization_application
    kiarabot_identity_invalidation_application
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
    kiarabot_postgres kiarabot_runtime_resource_application libpqxx::pqxx)
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
    tests/database_test.cpp tests/common_work_checks.cpp tests/typed_work_checks.cpp)
  target_link_libraries(kiarabot_postgres_database_test PRIVATE kiarabot_postgres libpqxx::pqxx)
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
