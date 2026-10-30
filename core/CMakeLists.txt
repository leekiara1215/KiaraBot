add_executable(kiarabot-core src/main.cpp)
target_compile_features(kiarabot-core PRIVATE cxx_std_20)
target_include_directories(kiarabot-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(kiarabot-core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(kiarabot-core-smoke tests/core_smoke.cpp)
target_include_directories(kiarabot-core-smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME core_smoke COMMAND kiarabot-core-smoke)

add_executable(kiarabot-runtime-test tests/runtime_test.cpp)
target_compile_features(kiarabot-runtime-test PRIVATE cxx_std_20)
target_include_directories(kiarabot-runtime-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(kiarabot-runtime-test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME core_runtime COMMAND kiarabot-runtime-test)

add_executable(kiarabot-plugin-delivery-test tests/plugin_delivery_test.cpp)
target_compile_features(kiarabot-plugin-delivery-test PRIVATE cxx_std_20)
target_include_directories(kiarabot-plugin-delivery-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(kiarabot-plugin-delivery-test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME core_plugin_delivery COMMAND kiarabot-plugin-delivery-test)
if(KIARABOT_GENERATE_PROTO)
  add_executable(kiarabot-host-validation-test tests/host_validation_test.cpp)
  target_include_directories(kiarabot-host-validation-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/cpp)
  target_link_libraries(kiarabot-host-validation-test PRIVATE kiarabot_proto)
  add_test(NAME host_validation COMMAND kiarabot-host-validation-test)
endif()

if(UNIX)
  target_sources(kiarabot-core PRIVATE src/plugin_process.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(kiarabot-core PRIVATE Threads::Threads)
  find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
  add_test(NAME core_process_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/process_lifecycle_test.py
      $<TARGET_FILE:kiarabot-core>)
  set_tests_properties(core_process_lifecycle PROPERTIES TIMEOUT 30)
endif()
if(UNIX AND KIARABOT_GENERATE_PROTO)
  add_test(NAME core_grpc_process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/grpc_process_test.py
      $<TARGET_FILE:kiarabot-core> $<$<BOOL:${KIARABOT_ENABLE_INFRASTRUCTURE}>:--sqlite>)
  set_tests_properties(core_grpc_process PROPERTIES TIMEOUT 30)
endif()

add_executable(kiarabot-runtime-store-test tests/runtime_store_test.cpp)
target_include_directories(kiarabot-runtime-store-test PRIVATE include)
add_test(NAME core_runtime_store COMMAND kiarabot-runtime-store-test)
