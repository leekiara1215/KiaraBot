# Host maintenance inner model/application; no wire, database or process implementation here.
add_library(kiarabot_host_maintenance STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/host_maintenance.cpp)
set_property(TARGET kiarabot_host_maintenance PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_host_maintenance PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_compile_features(kiarabot_host_maintenance PUBLIC cxx_std_20)
target_compile_options(kiarabot_host_maintenance PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)

if(KIARABOT_ENABLE_INFRASTRUCTURE AND KIARABOT_GENERATE_PROTO AND UNIX)
  add_library(kiarabot_host_maintenance_sqlite STATIC
    ${CMAKE_SOURCE_DIR}/infrastructure/src/host_maintenance_sqlite.cpp)
  target_include_directories(kiarabot_host_maintenance_sqlite PUBLIC
    ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/infrastructure/include)
  target_link_libraries(kiarabot_host_maintenance_sqlite PUBLIC kiarabot_host_maintenance
    PRIVATE kiarabot_host_journal kiarabot_core_json)
  target_compile_options(kiarabot_host_maintenance_sqlite PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_executable(kiarabot_host_maintenance_test
    ${CMAKE_SOURCE_DIR}/infrastructure/tests/host_maintenance_test.cpp)
  target_link_libraries(kiarabot_host_maintenance_test PRIVATE kiarabot_host_maintenance_sqlite
    kiarabot_host_service kiarabot_host_journal kiarabot_host_process)
  target_compile_options(kiarabot_host_maintenance_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p08_host_maintenance COMMAND kiarabot_host_maintenance_test)
  set_tests_properties(p08_host_maintenance PROPERTIES LABELS "p08;host;integration;fault" TIMEOUT 30)
endif()
