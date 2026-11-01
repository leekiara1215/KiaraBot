cmake_minimum_required(VERSION 3.24)
project(kiarabot LANGUAGES CXX)

option(KIARABOT_GENERATE_PROTO "Generate protobuf and gRPC sources" OFF)
set(KIARABOT_PROTOBUF_MIN_VERSION "25.0" CACHE STRING "Minimum Protobuf version")
set(KIARABOT_GRPC_MIN_VERSION "1.60.0" CACHE STRING "Minimum gRPC version")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "KiaraBot requires GCC 13 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "KiaraBot requires Clang 16 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
  message(FATAL_ERROR "KiaraBot requires AppleClang 15 or newer")
endif()

enable_testing()
add_subdirectory(core)

if(KIARABOT_GENERATE_PROTO)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  if(Protobuf_VERSION VERSION_LESS KIARABOT_PROTOBUF_MIN_VERSION)
    message(FATAL_ERROR "Protobuf ${KIARABOT_PROTOBUF_MIN_VERSION}+ is required (found ${Protobuf_VERSION})")
  endif()
  if(gRPC_VERSION VERSION_LESS KIARABOT_GRPC_MIN_VERSION)
    message(FATAL_ERROR "gRPC ${KIARABOT_GRPC_MIN_VERSION}+ is required (found ${gRPC_VERSION})")
  endif()
  if(TARGET protobuf::protoc)
  set(PROTOC_EXECUTABLE "$<TARGET_FILE:protobuf::protoc>")
else()
  find_program(PROTOC_EXECUTABLE protoc REQUIRED)
endif()
  if(TARGET gRPC::grpc_cpp_plugin)
    set(GRPC_CPP_PLUGIN_EXECUTABLE "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
  else()
    find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
  endif()
  set(KIARABOT_PROTOBUF_INCLUDE_ARGS)
  foreach(PROTOBUF_INCLUDE_DIR IN LISTS Protobuf_INCLUDE_DIRS)
    list(APPEND KIARABOT_PROTOBUF_INCLUDE_ARGS
      --proto_path=${PROTOBUF_INCLUDE_DIR})
  endforeach()
  set(KIARABOT_PROTO_OUT ${CMAKE_BINARY_DIR}/generated/cpp)
  set(KIARABOT_DESCRIPTOR ${CMAKE_BINARY_DIR}/generated/kiarabot-v1.pb)
  set(KIARABOT_PROTO_FILES
    common.proto events.proto actions.proto adapters.proto plugins.proto services.proto
    policy.proto platform.proto identity.proto runtime.proto host.proto scheduler.proto model.proto)
  set(KIARABOT_PROTO_INPUTS)
  set(KIARABOT_PROTO_OUTPUTS)
  set(KIARABOT_PROTO_SOURCES)
  foreach(PROTO_FILE IN LISTS KIARABOT_PROTO_FILES)
    string(REPLACE ".proto" "" PROTO_STEM ${PROTO_FILE})
    list(APPEND KIARABOT_PROTO_OUTPUTS
      ${KIARABOT_PROTO_OUT}/kiarabot/v1/${PROTO_STEM}.pb.cc
      ${KIARABOT_PROTO_OUT}/kiarabot/v1/${PROTO_STEM}.pb.h
      ${KIARABOT_PROTO_OUT}/kiarabot/v1/${PROTO_STEM}.grpc.pb.cc
      ${KIARABOT_PROTO_OUT}/kiarabot/v1/${PROTO_STEM}.grpc.pb.h)
    list(APPEND KIARABOT_PROTO_SOURCES
      ${CMAKE_SOURCE_DIR}/proto/kiarabot/v1/${PROTO_FILE})
    list(APPEND KIARABOT_PROTO_INPUTS kiarabot/v1/${PROTO_FILE})
  endforeach()
  add_custom_command(
    OUTPUT ${KIARABOT_PROTO_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KIARABOT_PROTO_OUT}
    COMMAND ${PROTOC_EXECUTABLE}
      --proto_path=${CMAKE_SOURCE_DIR}/proto
      ${KIARABOT_PROTOBUF_INCLUDE_ARGS}
      --cpp_out=${KIARABOT_PROTO_OUT}
      --grpc_out=${KIARABOT_PROTO_OUT}
      --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
      ${KIARABOT_PROTO_INPUTS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/proto
    DEPENDS ${KIARABOT_PROTO_SOURCES}
    COMMENT "Generating C++ protobuf and gRPC sources"
  )
  add_custom_command(
    OUTPUT ${KIARABOT_DESCRIPTOR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${PROTOC_EXECUTABLE}
      --proto_path=${CMAKE_SOURCE_DIR}/proto
      ${KIARABOT_PROTOBUF_INCLUDE_ARGS}
      --descriptor_set_out=${KIARABOT_DESCRIPTOR}
      --include_imports
      ${KIARABOT_PROTO_INPUTS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/proto
    DEPENDS ${KIARABOT_PROTO_SOURCES}
    COMMENT "Generating KiaraBot protocol descriptor"
  )
  add_library(kiarabot_proto STATIC ${KIARABOT_PROTO_OUTPUTS})
  add_custom_target(kiarabot_proto_generate DEPENDS ${KIARABOT_PROTO_OUTPUTS} ${KIARABOT_DESCRIPTOR})
  add_dependencies(kiarabot_proto kiarabot_proto_generate)
  target_include_directories(kiarabot_proto PUBLIC ${KIARABOT_PROTO_OUT})
  target_link_libraries(kiarabot_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
  include(core/cmake/bridge.cmake)
  add_subdirectory(control/api)
  add_executable(kiarabot-control-application-test core/tests/control_application_test.cpp)
  target_link_libraries(kiarabot-control-application-test PRIVATE kiarabot_bridge)
  add_test(NAME control_application COMMAND kiarabot-control-application-test)
  target_compile_options(kiarabot_bridge PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
  add_executable(kiarabot-bridge-test core/tests/bridge_server_test.cpp)
  target_link_libraries(kiarabot-bridge-test PRIVATE kiarabot_bridge)
  add_test(NAME core_bridge COMMAND kiarabot-bridge-test)
  set_tests_properties(core_bridge PROPERTIES TIMEOUT 30)
  target_link_libraries(kiarabot-reference-core PRIVATE kiarabot_bridge)
  target_compile_definitions(kiarabot-reference-core PRIVATE KIARABOT_HAS_GRPC=1)
  add_dependencies(kiarabot-reference-core kiarabot_proto)
  add_executable(kiarabot-model-host-test core/tests/model_host_test.cpp)
  target_link_libraries(kiarabot-model-host-test PRIVATE kiarabot_bridge)
  add_test(NAME model_host COMMAND kiarabot-model-host-test)
  set_tests_properties(model_host PROPERTIES TIMEOUT 10)
  add_executable(kiarabot_target_roundtrip proto/tests/target_roundtrip.cc)
  target_link_libraries(kiarabot_target_roundtrip PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,kiarabot_proto>")
  add_executable(kiarabot_proto_roundtrip proto/tests/roundtrip.cc)
  target_link_libraries(kiarabot_proto_roundtrip PRIVATE kiarabot_proto)
  target_include_directories(kiarabot_proto_roundtrip PRIVATE ${KIARABOT_PROTO_OUT})
  add_test(NAME proto_roundtrip COMMAND kiarabot_proto_roundtrip)
  add_executable(kiarabot_descriptor_compatibility proto/tests/descriptor_compatibility.cc)
  target_link_libraries(kiarabot_descriptor_compatibility PRIVATE protobuf::libprotobuf)
  add_test(NAME proto_descriptor_exists
    COMMAND ${CMAKE_COMMAND}
      -DVERIFY_FILE=${KIARABOT_DESCRIPTOR}
      -P ${CMAKE_SOURCE_DIR}/cmake/verify_file.cmake)
endif()

option(KIARABOT_ENABLE_INFRASTRUCTURE "Build infrastructure adapters and tests" OFF)
if(KIARABOT_ENABLE_INFRASTRUCTURE)
  add_subdirectory(infrastructure "${CMAKE_BINARY_DIR}/reference/infrastructure")
  if(KIARABOT_GENERATE_PROTO)
    target_link_libraries(kiarabot-reference-core PRIVATE kiarabot_sqlite_runtime kiarabot_model kiarabot_time_zone kiarabot_runtime_log kiarabot_message_projection)
    target_compile_definitions(kiarabot-reference-core PRIVATE KIARABOT_HAS_SQLITE=1)
    add_executable(kiarabot-control-service-test core/tests/control_service_test.cpp)
    target_link_libraries(kiarabot-control-service-test PRIVATE kiarabot_control kiarabot_sqlite_runtime)
    add_test(NAME control_service COMMAND kiarabot-control-service-test)
    add_executable(kiarabot-adapter-ingress-test core/tests/adapter_ingress_test.cpp)
    target_link_libraries(kiarabot-adapter-ingress-test PRIVATE kiarabot_bridge kiarabot_sqlite_runtime)
    add_test(NAME adapter_ingress COMMAND kiarabot-adapter-ingress-test)
    add_executable(kiarabot-action-router-test core/tests/action_router_test.cpp)
    target_link_libraries(kiarabot-action-router-test PRIVATE kiarabot_bridge kiarabot_sqlite_runtime)
    add_test(NAME action_router COMMAND kiarabot-action-router-test)
    add_executable(kiarabot-host-dispatcher-test core/tests/host_dispatcher_test.cpp)
    target_link_libraries(kiarabot-host-dispatcher-test PRIVATE kiarabot_bridge kiarabot_sqlite_runtime)
    add_test(NAME host_dispatcher COMMAND kiarabot-host-dispatcher-test)
    add_executable(kiarabot-task-scheduler-test core/tests/task_scheduler_test.cpp)
    target_link_libraries(kiarabot-task-scheduler-test PRIVATE kiarabot_bridge kiarabot_sqlite_runtime kiarabot_time_zone)
    add_test(NAME task_scheduler COMMAND kiarabot-task-scheduler-test)
    set_tests_properties(task_scheduler PROPERTIES TIMEOUT 20)
    set_tests_properties(adapter_ingress PROPERTIES TIMEOUT 30)
    add_executable(kiarabot-plugin-hub-test core/tests/plugin_hub_test.cpp)
    target_link_libraries(kiarabot-plugin-hub-test PRIVATE kiarabot_bridge kiarabot_sqlite_runtime)
    add_test(NAME plugin_hub COMMAND kiarabot-plugin-hub-test)
    set_tests_properties(plugin_hub PROPERTIES TIMEOUT 30)
  endif()
endif()

# Existing behavior assertions are retained as reference, never P02 acceptance.
get_property(KIARABOT_REFERENCE_TESTS DIRECTORY PROPERTY TESTS)
foreach(TEST_NAME IN LISTS KIARABOT_REFERENCE_TESTS)
  set_tests_properties(${TEST_NAME} PROPERTIES LABELS "reference")
endforeach()

if(KIARABOT_GENERATE_PROTO)
  include(cmake/platform.cmake)
  include(cmake/control-auth.cmake)
  add_subdirectory(dependencies/architecture/smoke)
  add_subdirectory(infrastructure/postgres)
  add_subdirectory(infrastructure/identity_crypto)
  include(cmake/identity-bootstrap.cmake)
  include(cmake/identity-security.cmake)
endif()

include(cmake/architecture-graph.cmake)
kiarabot_export_architecture_graph()
