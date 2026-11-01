# Trusted outer composition; no platform/crypto/PG dependency enters the pure application.
add_library(kiarabot_identity_security_clock STATIC
  ${CMAKE_SOURCE_DIR}/core/app/identity_security_clock.cpp)
target_include_directories(kiarabot_identity_security_clock PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/core/app)
target_link_libraries(kiarabot_identity_security_clock PRIVATE Threads::Threads)
kiarabot_platform_warnings(kiarabot_identity_security_clock)

target_sources(kiarabot-identity PRIVATE ${CMAKE_SOURCE_DIR}/core/app/identity_composition.cpp)
# RPC already owns the service-process/Admin/issuer link dependencies. Keep their only
# occurrences after their users rather than emitting duplicate static archives.
get_target_property(IDENTITY_PRODUCTION_LINKS kiarabot-identity LINK_LIBRARIES)
list(REMOVE_ITEM IDENTITY_PRODUCTION_LINKS kiarabot_service_process)
set_property(TARGET kiarabot-identity PROPERTY LINK_LIBRARIES "${IDENTITY_PRODUCTION_LINKS}")
target_link_libraries(kiarabot-identity PRIVATE
  kiarabot_identity_security_clock kiarabot_identity_security_application
  kiarabot_identity_rpc kiarabot_identity_crypto kiarabot_identity_verification_keys_application)

add_executable(kiarabot_identity_keys_daemon_client
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_keys_daemon_client.cpp)
target_link_libraries(kiarabot_identity_keys_daemon_client PRIVATE kiarabot_target_codec)
target_include_directories(kiarabot_identity_keys_daemon_client SYSTEM PRIVATE
  $<TARGET_PROPERTY:OpenSSL::Crypto,INTERFACE_INCLUDE_DIRECTORIES>)
kiarabot_platform_warnings(kiarabot_identity_keys_daemon_client)

add_executable(kiarabot_identity_security_clock_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_security_clock_test.cpp)
target_link_libraries(kiarabot_identity_security_clock_test PRIVATE
  kiarabot_identity_security_clock Threads::Threads)
target_compile_options(kiarabot_identity_security_clock_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
kiarabot_platform_warnings(kiarabot_identity_security_clock_test)
add_test(NAME p04_identity_security_clock COMMAND kiarabot_identity_security_clock_test)
set_tests_properties(p04_identity_security_clock PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)

if(UNIX)
  foreach(KEYS_MODE allowed denied)
    set(KEYS_DENIAL_ARGUMENT)
    if(KEYS_MODE STREQUAL "denied")
      set(KEYS_DENIAL_ARGUMENT --test-argument=--keys-no-grant)
    endif()
    add_test(NAME p04_identity_daemon_keys_${KEYS_MODE} COMMAND "${KIARABOT_TARGET_PYTHON}"
      "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
      --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
      --initializer $<TARGET_FILE:kiarabot-pg-initialize>
      --test "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_daemon_http_test.py"
      --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
      --test-argument "${KIARABOT_TEST_PG_BIN}"
      --test-argument $<TARGET_FILE:kiarabot-identity>
      --test-argument $<TARGET_FILE:kiarabot-control>
      --test-argument=--keys-client
      --test-argument $<TARGET_FILE:kiarabot_identity_keys_daemon_client>
      ${KEYS_DENIAL_ARGUMENT})
    set_tests_properties(p04_identity_daemon_keys_${KEYS_MODE} PROPERTIES
      LABELS "p04;integration;security;fault" TIMEOUT 120
      ENVIRONMENT "PATH=${BOOTSTRAP_OPENSSL_BIN_DIR}:${BOOTSTRAP_PYTHON_BIN_DIR}:$ENV{PATH}")
  endforeach()
  add_test(NAME p04_identity_daemon_http COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_daemon_http_test.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-control>)
  set_tests_properties(p04_identity_daemon_http PROPERTIES
    LABELS "p04;integration;security;fault" TIMEOUT 120
    ENVIRONMENT "PATH=${BOOTSTRAP_OPENSSL_BIN_DIR}:${BOOTSTRAP_PYTHON_BIN_DIR}:$ENV{PATH}")
  add_test(NAME p04_identity_daemon_tls_expiry COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_daemon_http_test.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-control>
    --test-argument=--own-tls-expiry)
  set_tests_properties(p04_identity_daemon_tls_expiry PROPERTIES
    LABELS "p04;integration;security;fault" TIMEOUT 120
    ENVIRONMENT "PATH=${BOOTSTRAP_OPENSSL_BIN_DIR}:${BOOTSTRAP_PYTHON_BIN_DIR}:$ENV{PATH}")
  add_test(NAME p04_identity_daemon_tls_cross_expiry COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_daemon_http_test.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}"
    --test-argument $<TARGET_FILE:kiarabot-identity>
    --test-argument $<TARGET_FILE:kiarabot-control>
    --test-argument=--own-tls-cross-expiry)
  set_tests_properties(p04_identity_daemon_tls_cross_expiry PROPERTIES
    LABELS "p04;integration;security;fault" TIMEOUT 120
    ENVIRONMENT "PATH=${BOOTSTRAP_OPENSSL_BIN_DIR}:${BOOTSTRAP_PYTHON_BIN_DIR}:$ENV{PATH}")
endif()
