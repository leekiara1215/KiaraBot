# Control's outer HTTPS/mTLS boundary has no direct business database access.
find_package(Boost CONFIG REQUIRED)
add_library(kiarabot_control_auth STATIC
  ${CMAKE_SOURCE_DIR}/control/api/src/target/identity_auth_client.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/https_endpoint.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/owner_gateway.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/owner_clients.cpp)
target_include_directories(kiarabot_control_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
target_link_libraries(kiarabot_control_auth PUBLIC kiarabot_service_process kiarabot_proto
  PRIVATE kiarabot_target_codec kiarabot_core_json Boost::headers OpenSSL::SSL OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_auth)
target_link_libraries(kiarabot-control PRIVATE kiarabot_control_auth)

add_executable(kiarabot_control_owner_gateway_test
  ${CMAKE_SOURCE_DIR}/control/api/tests/owner_gateway_test.cpp)
target_link_libraries(kiarabot_control_owner_gateway_test PRIVATE kiarabot_control_auth)
kiarabot_platform_warnings(kiarabot_control_owner_gateway_test)
add_test(NAME p12_control_owner_gateway COMMAND kiarabot_control_owner_gateway_test)
set_tests_properties(p12_control_owner_gateway PROPERTIES LABELS "p12;unit" TIMEOUT 30)

add_executable(kiarabot_control_identity_fixture
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_fixture.cpp)
target_link_libraries(kiarabot_control_identity_fixture PRIVATE kiarabot_proto OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_identity_fixture)
add_executable(kiarabot_control_model_fixture
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/model_fixture.cpp)
target_link_libraries(kiarabot_control_model_fixture PRIVATE kiarabot_proto OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_model_fixture)
add_test(NAME p04_control_auth_https COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/auth_https_test.py
  $<TARGET_FILE:kiarabot-control> $<TARGET_FILE:kiarabot_control_identity_fixture>
  $<TARGET_FILE:kiarabot_control_model_fixture>)
set_tests_properties(p04_control_auth_https PROPERTIES
  LABELS "p04;integration;fault" TIMEOUT 90 RESOURCE_LOCK control_https RUN_SERIAL TRUE
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
add_test(NAME p11_control_model_rest COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/p11_control_model_rest.py
  $<TARGET_FILE:kiarabot-control> $<TARGET_FILE:kiarabot_control_identity_fixture>
  $<TARGET_FILE:kiarabot_control_model_fixture>)
set_tests_properties(p11_control_model_rest PROPERTIES
  LABELS "p11;p12;control;integration;fault" TIMEOUT 90 RESOURCE_LOCK control_https RUN_SERIAL TRUE
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
add_test(NAME p12_audit_logs_rest COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/p12c_audit_logs.py
  $<TARGET_FILE:kiarabot-control> $<TARGET_FILE:kiarabot_control_identity_fixture>
  $<TARGET_FILE:kiarabot_control_model_fixture>)
set_tests_properties(p12_audit_logs_rest PROPERTIES
  LABELS "p12;control;integration;fault" TIMEOUT 90 RESOURCE_LOCK control_https RUN_SERIAL TRUE
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
