# Trusted offline composition, never an RPC or a pure application dependency.
if(UNIX)
  add_executable(kiarabot-identity-bootstrap
    ${CMAKE_SOURCE_DIR}/core/app/identity_bootstrap_main.cpp)
  target_link_libraries(kiarabot-identity-bootstrap PRIVATE
    kiarabot_service_process kiarabot_target_codec kiarabot_identity_crypto
    kiarabot_postgres kiarabot_identity_admin_application kiarabot_core_json)
  # OpenSSL linkage is already required by the outer adapters; expose its direct-use headers.
  target_include_directories(kiarabot-identity-bootstrap SYSTEM PRIVATE
    $<TARGET_PROPERTY:OpenSSL::Crypto,INTERFACE_INCLUDE_DIRECTORIES>)
  kiarabot_platform_warnings(kiarabot-identity-bootstrap)
  set_target_properties(kiarabot-identity-bootstrap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

  get_filename_component(BOOTSTRAP_PYTHON_BIN_DIR "${KIARABOT_TARGET_PYTHON}" DIRECTORY)
  get_filename_component(BOOTSTRAP_OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
  add_test(NAME p04_identity_bootstrap COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>
    --test "${CMAKE_SOURCE_DIR}/core/tests/platform/identity_bootstrap_test.py"
    --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
    --test-argument "${KIARABOT_TEST_PG_BIN}")
  set_tests_properties(p04_identity_bootstrap PROPERTIES
    LABELS "p04;integration;security;fault" TIMEOUT 120
    ENVIRONMENT "PATH=${BOOTSTRAP_OPENSSL_BIN_DIR}:${BOOTSTRAP_PYTHON_BIN_DIR}:$ENV{PATH}")
endif()
