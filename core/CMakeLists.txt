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
