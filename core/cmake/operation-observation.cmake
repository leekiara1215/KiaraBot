add_library(kiarabot_operation_observation_application STATIC
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/target/operation_observation_application.cpp)
set_property(TARGET kiarabot_operation_observation_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_operation_observation_application PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_features(kiarabot_operation_observation_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_operation_observation_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_operation_observation_application_test
  ${CMAKE_CURRENT_LIST_DIR}/../tests/architecture/operation_observation_application_test.cpp)
target_link_libraries(kiarabot_operation_observation_application_test PRIVATE
  kiarabot_operation_observation_application)
target_compile_options(kiarabot_operation_observation_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -UNDEBUG>)
add_test(NAME ws03_operation_observation_application
  COMMAND kiarabot_operation_observation_application_test)
set_tests_properties(ws03_operation_observation_application PROPERTIES
  LABELS "ws03;unit;security" TIMEOUT 10)
