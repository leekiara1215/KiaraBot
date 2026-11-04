# STR06 Model streaming transport slice: strict codec + ChatStream/ListOperationChanges RPC.
# Included from cmake/platform.cmake after all RPC targets and the kiarabot-model composition
# target exist. Owned by the streaming work stream; the include itself is owned by the lead.
find_package(Threads REQUIRED)

add_library(kiarabot_model_stream_codec STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_stream_codec.cpp)
target_include_directories(kiarabot_model_stream_codec PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_model_stream_codec)
target_link_libraries(kiarabot_model_stream_codec PUBLIC kiarabot_proto
  PRIVATE kiarabot_target_codec)

target_sources(kiarabot_model_rpc PRIVATE
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_stream_rpc.cpp)
target_link_libraries(kiarabot_model_rpc PUBLIC kiarabot_model_stream_codec
  kiarabot_operation_observation_rpc PRIVATE Threads::Threads)

# Model composition holds the Model-audience Runtime identity client and the pure observation
# application; record the dependency explicitly instead of relying on include order.
target_link_libraries(kiarabot-model PRIVATE kiarabot_runtime_identity_client)

add_executable(kiarabot_model_stream_codec_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_stream_codec_test.cpp)
target_link_libraries(kiarabot_model_stream_codec_test PRIVATE kiarabot_model_stream_codec)
kiarabot_platform_warnings(kiarabot_model_stream_codec_test)
add_test(NAME p11_streaming_model_codec COMMAND kiarabot_model_stream_codec_test)
set_tests_properties(p11_streaming_model_codec PROPERTIES LABELS "p11;streaming;unit" TIMEOUT 20)

add_executable(kiarabot_model_stream_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_stream_rpc_test.cpp)
target_link_libraries(kiarabot_model_stream_rpc_test PRIVATE
  kiarabot_model_rpc kiarabot_model_gateway_application)
kiarabot_platform_warnings(kiarabot_model_stream_rpc_test)
add_test(NAME p11_streaming_model_rpc COMMAND kiarabot_model_stream_rpc_test)
set_tests_properties(p11_streaming_model_rpc PROPERTIES
  LABELS "p11;streaming;integration" TIMEOUT 30)

# Real owned PG/TLS/Identity/HTTP fixture for the production Model ChatStream RPC and operation
# journal. argv[5..8] are identity-bootstrap, PG bin, identity and model binaries.
add_test(NAME p11_streaming_model_rpc_e2e COMMAND "${KIARABOT_TARGET_PYTHON}"
  "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
  --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
  --initializer $<TARGET_FILE:kiarabot-pg-initialize>
  --test "${CMAKE_SOURCE_DIR}/core/tests/platform/model_stream_rpc_e2e_test.py"
  --test-argument $<TARGET_FILE:kiarabot-identity-bootstrap>
  --test-argument "${KIARABOT_TEST_PG_BIN}"
  --test-argument $<TARGET_FILE:kiarabot-identity>
  --test-argument $<TARGET_FILE:kiarabot-model>)
set_tests_properties(p11_streaming_model_rpc_e2e PROPERTIES
  LABELS "p11;streaming;integration;security" TIMEOUT 180
  ENVIRONMENT "PATH=${RUNTIME_E2E_OPENSSL_BIN_DIR}:${RUNTIME_E2E_PYTHON_BIN_DIR}:$ENV{PATH};PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated")
