# P02 production libraries never inherit the historical reference include directory.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
find_package(Threads REQUIRED)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

function(kiarabot_platform_warnings TARGET_NAME)
  target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)
  target_compile_options(${TARGET_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
endfunction()

# Wire conversion belongs to the outer layer; pure application libraries never link it.
add_library(kiarabot_target_codec STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/target_codec.cpp)
target_include_directories(kiarabot_target_codec PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_target_codec PUBLIC kiarabot_proto)
kiarabot_platform_warnings(kiarabot_target_codec)
add_executable(kiarabot_target_codec_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/target_codec_test.cpp)
target_link_libraries(kiarabot_target_codec_test PRIVATE kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_target_codec_test)
add_test(NAME p04_target_codec COMMAND kiarabot_target_codec_test)
set_tests_properties(p04_target_codec PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)

foreach(SERVICE identity runtime plugin_host scheduler model)
  set(APPLICATION_SOURCE "${SERVICE}")
  if(SERVICE STREQUAL "plugin_host")
    set(APPLICATION_SOURCE host)
  endif()
  add_library(kiarabot_${SERVICE}_application STATIC
    ${CMAKE_SOURCE_DIR}/core/src/application/target/${APPLICATION_SOURCE}_application.cpp)
  target_include_directories(kiarabot_${SERVICE}_application PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
  kiarabot_platform_warnings(kiarabot_${SERVICE}_application)
endforeach()

add_library(kiarabot_control_application STATIC
  ${CMAKE_SOURCE_DIR}/control/api/src/application/target/application.cpp)
target_include_directories(kiarabot_control_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
kiarabot_platform_warnings(kiarabot_control_application)

add_executable(kiarabot_service_applications_test
  ${CMAKE_SOURCE_DIR}/core/tests/architecture/service_applications_test.cpp)
target_link_libraries(kiarabot_service_applications_test PRIVATE
  kiarabot_identity_application kiarabot_runtime_application kiarabot_plugin_host_application
  kiarabot_scheduler_application kiarabot_model_application)
kiarabot_platform_warnings(kiarabot_service_applications_test)
add_test(NAME p02_service_applications COMMAND kiarabot_service_applications_test)
set_tests_properties(p02_service_applications PROPERTIES LABELS "p02;unit" TIMEOUT 10)

add_executable(kiarabot_control_application_target_test
  ${CMAKE_SOURCE_DIR}/control/api/tests/architecture/application_test.cpp)
target_link_libraries(kiarabot_control_application_target_test PRIVATE kiarabot_control_application)
kiarabot_platform_warnings(kiarabot_control_application_target_test)
add_test(NAME p02_control_application COMMAND kiarabot_control_application_target_test)
set_tests_properties(p02_control_application PROPERTIES LABELS "p02;unit" TIMEOUT 10)

add_library(kiarabot_service_config STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_config.cpp)
target_include_directories(kiarabot_service_config PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_service_config PRIVATE OpenSSL::SSL OpenSSL::Crypto kiarabot_core_json)
kiarabot_platform_warnings(kiarabot_service_config)
add_executable(kiarabot_service_config_driver
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_config_driver.cpp)
target_link_libraries(kiarabot_service_config_driver PRIVATE kiarabot_service_config)
kiarabot_platform_warnings(kiarabot_service_config_driver)
add_test(NAME p02_service_config COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_config_test.py
  $<TARGET_FILE:kiarabot_service_config_driver> $<TARGET_FILE:kiarabot-identity>)
set_tests_properties(p02_service_config PROPERTIES LABELS "p02;unit" TIMEOUT 30)

# Create one translation unit per new inner header: no generated/protocol/JSON includes provided.
file(GLOB_RECURSE INNER_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/domain/*.hpp
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/application/*.hpp
  ${CMAKE_SOURCE_DIR}/core/include/kiarabot/core/ports/*.hpp
  ${CMAKE_SOURCE_DIR}/control/api/include/kiarabot/control/application/target/*.hpp)
set(HEADER_UNITS)
foreach(HEADER IN LISTS INNER_HEADERS)
  string(SHA256 HEADER_ID "${HEADER}")
  set(HEADER_UNIT "${CMAKE_BINARY_DIR}/inner-headers/${HEADER_ID}.cpp")
  file(GENERATE OUTPUT "${HEADER_UNIT}" CONTENT "#include \"${HEADER}\"\n")
  list(APPEND HEADER_UNITS "${HEADER_UNIT}")
endforeach()
add_library(kiarabot_inner_headers OBJECT ${HEADER_UNITS})
target_include_directories(kiarabot_inner_headers PRIVATE
  ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/control/api/include)
kiarabot_platform_warnings(kiarabot_inner_headers)

set(KIARABOT_TARGET_PYTHON "${CMAKE_SOURCE_DIR}/sdk/python/.venv/bin/python" CACHE FILEPATH
  "Locked SDK Python used for target generation and round-trip verification")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/python.stamp"
  COMMAND "${KIARABOT_TARGET_PYTHON}" "${CMAKE_SOURCE_DIR}/sdk/python/tools/generate_proto.py"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/generated/python.stamp"
  DEPENDS ${KIARABOT_PROTO_SOURCES} "${CMAKE_SOURCE_DIR}/sdk/python/tools/generate_proto.py"
  VERBATIM)
add_custom_target(kiarabot_python_proto_generate ALL
  DEPENDS "${CMAKE_BINARY_DIR}/generated/python.stamp")
add_test(NAME p01_target_roundtrip COMMAND ${CMAKE_COMMAND} -E env
  "PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated"
  "${KIARABOT_TARGET_PYTHON}" "${CMAKE_SOURCE_DIR}/proto/tests/target_contract_test.py"
  "${KIARABOT_DESCRIPTOR}" $<TARGET_FILE:kiarabot_target_roundtrip>)
set_tests_properties(p01_target_roundtrip PROPERTIES LABELS "p01;integration" TIMEOUT 60)

add_library(kiarabot_service_process STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_process.cpp
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/service_rpc.cpp
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/peer_certificate.cpp)
target_include_directories(kiarabot_service_process PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_service_process PRIVATE
  kiarabot_service_config kiarabot_proto Threads::Threads)
kiarabot_platform_warnings(kiarabot_service_process)

# Identity wire adaptation stays outer and borrows the pure application through its port.
add_library(kiarabot_identity_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/identity_rpc.cpp)
target_include_directories(kiarabot_identity_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_identity_rpc PUBLIC kiarabot_proto PRIVATE
  kiarabot_target_codec kiarabot_service_process kiarabot_identity_admin_application
  kiarabot_identity_delegation_application OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_identity_rpc)
add_executable(kiarabot_identity_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_rpc_test.cpp)
target_link_libraries(kiarabot_identity_rpc_test PRIVATE
  kiarabot_identity_rpc kiarabot_identity_crypto)
kiarabot_platform_warnings(kiarabot_identity_rpc_test)
add_test(NAME p04_identity_rpc COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/identity_rpc_test.py
  $<TARGET_FILE:kiarabot_identity_rpc_test>)
set_tests_properties(p04_identity_rpc PROPERTIES LABELS "p04;integration;security" TIMEOUT 60)

add_executable(kiarabot_peer_certificate_evidence_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/peer_certificate_test.cpp)
target_link_libraries(kiarabot_peer_certificate_evidence_test PRIVATE
  kiarabot_service_process kiarabot_proto)
kiarabot_platform_warnings(kiarabot_peer_certificate_evidence_test)
add_test(NAME p04_peer_certificate_evidence COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/peer_certificate_test.py
  $<TARGET_FILE:kiarabot_peer_certificate_evidence_test>)
set_tests_properties(p04_peer_certificate_evidence PROPERTIES
  LABELS "p04;integration;security" TIMEOUT 30)

add_executable(kiarabot_service_composition_driver
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_composition_driver.cpp)
target_link_libraries(kiarabot_service_composition_driver PRIVATE kiarabot_service_process kiarabot_proto)
kiarabot_platform_warnings(kiarabot_service_composition_driver)
add_test(NAME p03_service_composition COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_composition_test.py
  $<TARGET_FILE:kiarabot_service_composition_driver> $<TARGET_FILE:kiarabot_service_rpc_client>)
set_tests_properties(p03_service_composition PROPERTIES LABELS "p03;integration;fault" TIMEOUT 30)

foreach(SERVICE identity runtime plugin_host scheduler model)
  set(ENTRY "${SERVICE}")
  set(BINARY "${SERVICE}")
  if(SERVICE STREQUAL "plugin_host")
    set(ENTRY host)
    set(BINARY plugin-host)
  endif()
  add_executable(kiarabot-${BINARY} ${CMAKE_SOURCE_DIR}/core/app/${ENTRY}_main.cpp)
  target_link_libraries(kiarabot-${BINARY} PRIVATE
    kiarabot_${SERVICE}_application kiarabot_service_process)
  set_target_properties(kiarabot-${BINARY} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  kiarabot_platform_warnings(kiarabot-${BINARY})
endforeach()
add_executable(kiarabot-control ${CMAKE_SOURCE_DIR}/control/api/app/main.cpp)
# control_auth supplies the service_process dependency after its own archive.
target_link_libraries(kiarabot-control PRIVATE kiarabot_control_application)
set_target_properties(kiarabot-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
kiarabot_platform_warnings(kiarabot-control)
add_executable(kiarabot_service_rpc_client ${CMAKE_SOURCE_DIR}/core/tests/platform/service_rpc_client.cpp)
target_link_libraries(kiarabot_service_rpc_client PRIVATE kiarabot_proto)
kiarabot_platform_warnings(kiarabot_service_rpc_client)

find_program(KIARABOT_TEST_OPENSSL openssl REQUIRED)
get_filename_component(OPENSSL_BIN_DIR "${KIARABOT_TEST_OPENSSL}" DIRECTORY)
set(P02_BINARY_ARGUMENTS --control $<TARGET_FILE:kiarabot-control>
  --identity $<TARGET_FILE:kiarabot-identity> --runtime $<TARGET_FILE:kiarabot-runtime>
  --plugin-host $<TARGET_FILE:kiarabot-plugin-host> --scheduler $<TARGET_FILE:kiarabot-scheduler>
  --model $<TARGET_FILE:kiarabot-model>)
add_test(NAME p02_core_process COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_process_test.py
  $<TARGET_FILE:kiarabot-identity> $<TARGET_FILE:kiarabot-runtime> $<TARGET_FILE:kiarabot-plugin-host>
  $<TARGET_FILE:kiarabot-scheduler> $<TARGET_FILE:kiarabot-model> $<TARGET_FILE:kiarabot_service_rpc_client>)
set_tests_properties(p02_core_process PROPERTIES LABELS "p02;integration;fault" TIMEOUT 90)
foreach(GROUP lifecycle tls isolation)
  add_test(NAME architecture_p02_${GROUP} COMMAND "${KIARABOT_TARGET_PYTHON}"
    ${CMAKE_SOURCE_DIR}/tests/architecture/run.py --group ${GROUP}
    --openssl "${KIARABOT_TEST_OPENSSL}" ${P02_BINARY_ARGUMENTS})
  set_tests_properties(architecture_p02_${GROUP} PROPERTIES
    LABELS "p02;integration;fault" TIMEOUT 360)
endforeach()
foreach(FIXTURE fixture tls_fixture)
  add_test(NAME architecture_p02_${FIXTURE} COMMAND "${KIARABOT_TARGET_PYTHON}"
    ${CMAKE_SOURCE_DIR}/tests/architecture/${FIXTURE}_test.py --openssl "${KIARABOT_TEST_OPENSSL}")
  set(FIXTURE_LABELS "p02;unit")
  if(FIXTURE STREQUAL "tls_fixture")
    set(FIXTURE_LABELS "p02;integration")
  endif()
  set_tests_properties(architecture_p02_${FIXTURE} PROPERTIES LABELS "${FIXTURE_LABELS}" TIMEOUT 30)
endforeach()
add_test(NAME architecture_p02_oracle COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/support_test.py)
set_tests_properties(architecture_p02_oracle PROPERTIES LABELS "p02;unit" TIMEOUT 30)
add_test(NAME architecture_p02_references COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/check_references.py)
set_tests_properties(architecture_p02_references PROPERTIES LABELS "p02;unit;reference" TIMEOUT 10)
set_tests_properties(p02_service_config p02_core_process p03_service_composition p04_peer_certificate_evidence p04_identity_rpc PROPERTIES
  ENVIRONMENT "PATH=${OPENSSL_BIN_DIR}:$ENV{PATH}")
add_test(NAME p01_target_api COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/quality/check-target-api.sh)
set_tests_properties(p01_target_api PROPERTIES LABELS "p01;unit" TIMEOUT 30)

add_test(NAME architecture_p02_reference_manifest_test COMMAND "${KIARABOT_TARGET_PYTHON}"
  ${CMAKE_SOURCE_DIR}/tests/architecture/reference_test.py)
set_tests_properties(architecture_p02_reference_manifest_test PROPERTIES LABELS "p02;unit" TIMEOUT 10)
