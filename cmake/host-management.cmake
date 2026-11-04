# HOST-03/04 Host owner management: Host->Runtime admission gate client and the HostManagement
# owner mTLS RPC adapter. Wire-only adapters; no business rules, database or process ownership here.
add_library(kiarabot_host_runtime_client STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/host_runtime_client.cpp)
target_include_directories(kiarabot_host_runtime_client PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_host_runtime_client)
target_link_libraries(kiarabot_host_runtime_client PUBLIC kiarabot_proto kiarabot_target_codec)

add_executable(kiarabot_host_runtime_client_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/host_runtime_client_test.cpp)
target_link_libraries(kiarabot_host_runtime_client_test PRIVATE
  kiarabot_host_runtime_client)
kiarabot_platform_warnings(kiarabot_host_runtime_client_test)
add_test(NAME p08_host_runtime_client COMMAND kiarabot_host_runtime_client_test)
set_tests_properties(p08_host_runtime_client PROPERTIES
  LABELS "p08;host;unit;fault" TIMEOUT 30)

# HostManagementService owner mTLS adapter over the HOST-02 application; compiled for the final
# integration build. Wire/e2e coverage is not complete at the freeze point (see HANDOFF report).
add_library(kiarabot_host_management_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/host_management_rpc.cpp)
target_include_directories(kiarabot_host_management_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_host_management_rpc)
target_link_libraries(kiarabot_host_management_rpc PUBLIC kiarabot_proto kiarabot_target_codec
  kiarabot_host_maintenance kiarabot_operation_observation_rpc)
