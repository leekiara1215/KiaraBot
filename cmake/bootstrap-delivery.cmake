# BOOT-02 Runtime controlled bootstrap delivery. The root CMakeLists includes this fragment after
# control-auth.cmake behind an EXISTS guard; the coordinator later makes the include unconditional.
# The application TU belongs to the existing Runtime resource application library so the RPC
# boundary and composition root can use it without a second application library.
target_sources(kiarabot_runtime_resource_application PRIVATE
  ${CMAKE_SOURCE_DIR}/core/src/application/target/runtime_resources/bootstrap.cpp)

add_executable(kiarabot_runtime_bootstrap_delivery_test
  ${CMAKE_SOURCE_DIR}/core/tests/architecture/runtime_bootstrap_delivery_test.cpp)
target_link_libraries(kiarabot_runtime_bootstrap_delivery_test PRIVATE
  kiarabot_runtime_resource_application)
kiarabot_platform_warnings(kiarabot_runtime_bootstrap_delivery_test)
add_test(NAME p05_runtime_bootstrap_delivery COMMAND kiarabot_runtime_bootstrap_delivery_test)
set_tests_properties(p05_runtime_bootstrap_delivery PROPERTIES
  LABELS "p05;p06;boot;unit" TIMEOUT 30)

# BOOT-04 Control owner-gateway handler. The coordinator's control-auth.cmake registration is
# temporarily disabled while this interface slice lands; keep the single registration here so the
# bootstrap fragment owns its new TU. Remove any duplicate source entry in control-auth.cmake.
if(TARGET kiarabot_control_auth)
  target_sources(kiarabot_control_auth PRIVATE
    ${CMAKE_SOURCE_DIR}/control/api/src/target/owner_gateway/bootstrap.cpp)
  target_link_libraries(kiarabot_control_auth PRIVATE kiarabot_secret_file_delivery)
endif()

