# Control's outer HTTPS/mTLS boundary has no direct business database access.
find_package(Boost CONFIG REQUIRED)
add_library(kiarabot_control_auth STATIC
  ${CMAKE_SOURCE_DIR}/control/api/src/target/identity_auth_client.cpp
  ${CMAKE_SOURCE_DIR}/control/api/src/target/https_endpoint.cpp)
target_include_directories(kiarabot_control_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
target_link_libraries(kiarabot_control_auth PUBLIC kiarabot_service_process PRIVATE
  kiarabot_proto kiarabot_core_json Boost::headers OpenSSL::SSL OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_auth)
target_link_libraries(kiarabot-control PRIVATE kiarabot_control_auth)

add_executable(kiarabot_control_identity_fixture
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/identity_fixture.cpp)
target_link_libraries(kiarabot_control_identity_fixture PRIVATE kiarabot_proto OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_control_identity_fixture)
add_test(NAME p04_control_auth_https COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/auth_https_test.py
  $<TARGET_FILE:kiarabot-control> $<TARGET_FILE:kiarabot_control_identity_fixture>)
set_tests_properties(p04_control_auth_https PROPERTIES
  LABELS "p04;integration;fault" TIMEOUT 60
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
