# STR Provider streaming: only the pinned C parser is linked into production. Never invoke
# upstream's CMake project (it enables a reconnecting client and a different curl transport).
include(FetchContent)
set(KIARABOT_STREAM_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
file(READ "${KIARABOT_STREAM_ROOT}/dependencies/architecture/dependencies.lock.json"
  KIARABOT_STREAM_LOCK)
string(JSON KIARABOT_SSE_URL GET "${KIARABOT_STREAM_LOCK}" source-archives eventsource-c url)
string(JSON KIARABOT_SSE_SHA512 GET "${KIARABOT_STREAM_LOCK}" source-archives eventsource-c sha512)
FetchContent_Declare(kiarabot_eventsource
  URL "${KIARABOT_SSE_URL}"
  URL_HASH "SHA512=${KIARABOT_SSE_SHA512}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR kiarabot-parser-only-no-upstream-cmake)
FetchContent_MakeAvailable(kiarabot_eventsource)
foreach(KIARABOT_SSE_FILE LICENSE src/sse_parser.c include/eventsource/sse_parser.h)
  string(JSON KIARABOT_SSE_EXPECTED GET "${KIARABOT_STREAM_LOCK}"
    source-archives eventsource-c files-sha256 "${KIARABOT_SSE_FILE}")
  file(SHA256 "${kiarabot_eventsource_SOURCE_DIR}/${KIARABOT_SSE_FILE}" KIARABOT_SSE_ACTUAL)
  if(NOT KIARABOT_SSE_ACTUAL STREQUAL KIARABOT_SSE_EXPECTED)
    message(FATAL_ERROR "Locked SSE file checksum mismatch: ${KIARABOT_SSE_FILE}")
  endif()
endforeach()
set(KIARABOT_SSE_LICENSE_DIR "${CMAKE_BINARY_DIR}/third-party-licenses/eventsource-c")
file(MAKE_DIRECTORY "${KIARABOT_SSE_LICENSE_DIR}")
configure_file("${kiarabot_eventsource_SOURCE_DIR}/LICENSE"
  "${KIARABOT_SSE_LICENSE_DIR}/LICENSE" COPYONLY)
string(JSON KIARABOT_SSE_PROVENANCE GET "${KIARABOT_STREAM_LOCK}" source-archives eventsource-c)
file(GENERATE OUTPUT "${KIARABOT_SSE_LICENSE_DIR}/provenance.json"
  CONTENT "${KIARABOT_SSE_PROVENANCE}\n")
install(FILES "${KIARABOT_SSE_LICENSE_DIR}/LICENSE" "${KIARABOT_SSE_LICENSE_DIR}/provenance.json"
  DESTINATION share/kiarabot/licenses/eventsource-c COMPONENT model-streaming-licenses)
add_library(kiarabot_model_sse_c STATIC "${kiarabot_eventsource_SOURCE_DIR}/src/sse_parser.c")
set_target_properties(kiarabot_model_sse_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(kiarabot_model_sse_c SYSTEM PUBLIC
  "${kiarabot_eventsource_SOURCE_DIR}/include")
target_compile_options(kiarabot_model_sse_c PRIVATE
  $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)

target_sources(kiarabot_model_http_provider PRIVATE
  "${KIARABOT_STREAM_ROOT}/infrastructure/src/model_sse_parser.cpp"
  "${KIARABOT_STREAM_ROOT}/infrastructure/src/model_http_stream_provider.cpp")
target_link_libraries(kiarabot_model_http_provider PRIVATE kiarabot_model_sse_c utf8_range::utf8_validity)

add_executable(kiarabot_model_sse_parser_test
  "${KIARABOT_STREAM_ROOT}/infrastructure/tests/model_sse_parser_test.cpp")
target_include_directories(kiarabot_model_sse_parser_test PRIVATE
  "${KIARABOT_STREAM_ROOT}/infrastructure/src")
target_link_libraries(kiarabot_model_sse_parser_test PRIVATE kiarabot_model_http_provider)
add_test(NAME p11_streaming_sse_parser COMMAND kiarabot_model_sse_parser_test)
set_tests_properties(p11_streaming_sse_parser PROPERTIES LABELS "p11;streaming;unit" TIMEOUT 20)

if(UNIX)
  add_executable(kiarabot_model_http_stream_provider_test
    "${KIARABOT_STREAM_ROOT}/infrastructure/tests/model_http_stream_provider_test.cpp")
  target_link_libraries(kiarabot_model_http_stream_provider_test PRIVATE kiarabot_model_http_provider)
  add_test(NAME p11_streaming_http_provider COMMAND kiarabot_model_http_stream_provider_test)
  set_tests_properties(p11_streaming_http_provider PROPERTIES
    LABELS "p11;streaming;integration;fault" TIMEOUT 45)
endif()
foreach(KIARABOT_STREAM_TEST kiarabot_model_sse_parser_test kiarabot_model_http_stream_provider_test)
  if(TARGET ${KIARABOT_STREAM_TEST})
    target_compile_options(${KIARABOT_STREAM_TEST} PRIVATE
      $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)
  endif()
endforeach()

# Upstream tests are regressions of the locked dependency, never application implementation
# evidence. Four client tests use only fake transports; that support library has no production
# consumers and no upstream HTTP transport/reconnect loop is used by the Provider.
add_library(kiarabot_model_sse_client_test_support STATIC
  "${kiarabot_eventsource_SOURCE_DIR}/src/sse_client.c")
target_link_libraries(kiarabot_model_sse_client_test_support PUBLIC kiarabot_model_sse_c)
set_target_properties(kiarabot_model_sse_client_test_support PROPERTIES C_STANDARD 99)
foreach(KIARABOT_SSE_TEST parser_init parser_basic parser_fields parser_boundaries parser_limits
    client_basic client_reconnect client_policy client_lifecycle)
  add_executable(kiarabot_sse_upstream_${KIARABOT_SSE_TEST}
    "${kiarabot_eventsource_SOURCE_DIR}/tests/test_${KIARABOT_SSE_TEST}.c")
  target_link_libraries(kiarabot_sse_upstream_${KIARABOT_SSE_TEST}
    PRIVATE kiarabot_model_sse_client_test_support)
  add_test(NAME p11_streaming_upstream_${KIARABOT_SSE_TEST}
    COMMAND kiarabot_sse_upstream_${KIARABOT_SSE_TEST})
  set_tests_properties(p11_streaming_upstream_${KIARABOT_SSE_TEST}
    PROPERTIES LABELS "p11;streaming;dependency" TIMEOUT 10)
endforeach()
add_executable(kiarabot_sse_upstream_fuzz_smoke
  "${kiarabot_eventsource_SOURCE_DIR}/fuzz/fuzz_parser.c"
  "${kiarabot_eventsource_SOURCE_DIR}/fuzz/standalone_main.c")
target_link_libraries(kiarabot_sse_upstream_fuzz_smoke PRIVATE kiarabot_model_sse_c)
add_test(NAME p11_streaming_upstream_fuzz_smoke COMMAND kiarabot_sse_upstream_fuzz_smoke)
set_tests_properties(p11_streaming_upstream_fuzz_smoke PROPERTIES
  LABELS "p11;streaming;dependency" TIMEOUT 20)
