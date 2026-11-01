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

foreach(SERVICE identity runtime host scheduler model)
  add_library(kiarabot_${SERVICE}_application STATIC
    ${CMAKE_SOURCE_DIR}/core/src/application/target/${SERVICE}_application.cpp)
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
  kiarabot_identity_application kiarabot_runtime_application kiarabot_host_application
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
  ${CMAKE_SOURCE_DIR}/core/tests/platform/service_config_test.py $<TARGET_FILE:kiarabot_service_config_driver>)
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
  file(WRITE "${HEADER_UNIT}" "#include \"${HEADER}\"\n")
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
