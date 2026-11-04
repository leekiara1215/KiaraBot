# Pure Model application streaming slice; no RPC/HTTP/SQL dependency enters the inner target.
target_sources(kiarabot_model_gateway_application PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../src/application/target/model_gateway_application_stream.cpp")
find_package(Threads REQUIRED)
target_link_libraries(kiarabot_model_gateway_application PRIVATE Threads::Threads)
add_executable(kiarabot_model_stream_application_test
  "${CMAKE_CURRENT_LIST_DIR}/../tests/architecture/model_stream_application_test.cpp")
target_link_libraries(kiarabot_model_stream_application_test PRIVATE
  kiarabot_model_gateway_application Threads::Threads)
target_compile_options(kiarabot_model_stream_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-UNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
add_test(NAME p11_streaming_model_application COMMAND kiarabot_model_stream_application_test)
set_tests_properties(p11_streaming_model_application PROPERTIES
  LABELS "p11;streaming;unit;fault" TIMEOUT 30)
