# WS-12 v1.2 owner observation feed journals (logs/messages/tasks).
#
# Included from cmake/platform.cmake behind an EXISTS guard while the coordinator owns the shared
# aggregation files. Registration is deferred to the end of the root directory so the
# kiarabot_postgres adapter and the kiarabot-pg-initialize binary already exist.
# Pure application/RPC boundary for the v1.2 feed journals (owner authorization + wire mapping).
add_library(kiarabot_observation_feed_application STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/observation_feed_application.cpp)
target_include_directories(kiarabot_observation_feed_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_observation_feed_application PUBLIC kiarabot_proto)
kiarabot_platform_warnings(kiarabot_observation_feed_application)

add_library(kiarabot_identity_log_feed_application STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/identity_log_feed_application.cpp)
set_property(TARGET kiarabot_identity_log_feed_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_log_feed_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include
  PRIVATE ${CMAKE_SOURCE_DIR}/core/src/application/target)
target_link_libraries(kiarabot_identity_log_feed_application PUBLIC kiarabot_proto)
kiarabot_platform_warnings(kiarabot_identity_log_feed_application)

add_library(kiarabot_observation_feed_rpc STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/observation_feed_rpc.cpp)
target_include_directories(kiarabot_observation_feed_rpc PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_observation_feed_rpc PUBLIC kiarabot_proto
  kiarabot_observation_feed_application
  PRIVATE kiarabot_runtime_identity_client kiarabot_target_codec)
kiarabot_platform_warnings(kiarabot_observation_feed_rpc)

add_executable(kiarabot_observation_feed_application_test
  ${CMAKE_SOURCE_DIR}/core/tests/architecture/observation_feed_application_test.cpp)
target_link_libraries(kiarabot_observation_feed_application_test PRIVATE
  kiarabot_observation_feed_rpc)
kiarabot_platform_warnings(kiarabot_observation_feed_application_test)
add_test(NAME ws12_observation_feed_application COMMAND kiarabot_observation_feed_application_test)
set_tests_properties(ws12_observation_feed_application PROPERTIES LABELS "ws12;unit" TIMEOUT 60)

function(kiarabot_register_ws_feeds)
  if(NOT TARGET kiarabot_postgres)
    return()
  endif()
  target_sources(kiarabot_postgres PRIVATE
    ${CMAKE_SOURCE_DIR}/infrastructure/postgres/src/observation_feeds.cpp)
  if(NOT UNIX)
    return()
  endif()
  # The adapter's own find_package() lives in a subdirectory scope; re-find here so the imported
  # target is visible to this deferred root-scope registration.
  find_package(libpqxx 7.10.0 EXACT CONFIG REQUIRED)
  add_executable(kiarabot_postgres_observation_feed_test
    ${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/observation_feed_test.cpp)
  target_link_libraries(kiarabot_postgres_observation_feed_test PRIVATE
    kiarabot_postgres libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_observation_feed_test)
  add_test(NAME ws12_observation_feed_journal COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/infrastructure/postgres/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_observation_feed_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(ws12_observation_feed_journal PROPERTIES
    LABELS "ws12;integration;fault" TIMEOUT 180)
endfunction()
if(COMMAND cmake_language)
  cmake_language(DEFER CALL kiarabot_register_ws_feeds)
else()
  message(WARNING "CMake without cmake_language(DEFER): ws-feeds registration skipped")
endif()
