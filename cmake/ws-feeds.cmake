# WS-12 v1.2 owner observation feed journals (logs/messages/tasks).
#
# Included from cmake/platform.cmake behind an EXISTS guard while the coordinator owns the shared
# aggregation files. Registration is deferred to the end of the root directory so the
# kiarabot_postgres adapter and the kiarabot-pg-initialize binary already exist.
# Deterministic received-message feed identity (fixed namespace UUIDv5; contract constants).
# The SHA-1 derivation is a boundary dependency, so the implementation lives in infrastructure and
# the pure declaration stays in the domain header.
add_library(kiarabot_observation_feed_identity STATIC
  ${CMAKE_SOURCE_DIR}/infrastructure/src/observation_feed_identity.cpp)
target_include_directories(kiarabot_observation_feed_identity PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_observation_feed_identity PRIVATE OpenSSL::Crypto)
kiarabot_platform_warnings(kiarabot_observation_feed_identity)

add_executable(kiarabot_observation_feed_identity_test
  ${CMAKE_SOURCE_DIR}/core/tests/architecture/observation_feed_identity_test.cpp)
target_link_libraries(kiarabot_observation_feed_identity_test PRIVATE
  kiarabot_observation_feed_identity)
kiarabot_platform_warnings(kiarabot_observation_feed_identity_test)
add_test(NAME ws12_observation_feed_identity COMMAND kiarabot_observation_feed_identity_test)
set_tests_properties(ws12_observation_feed_identity PROPERTIES LABELS "ws12;unit" TIMEOUT 30)

# Pure application/RPC boundary for the v1.2 feed journals (owner authorization + wire mapping).
add_library(kiarabot_observation_feed_application STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/observation_feed_application.cpp)
set_property(TARGET kiarabot_observation_feed_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_observation_feed_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_observation_feed_application)

add_library(kiarabot_identity_log_feed_application STATIC
  ${CMAKE_SOURCE_DIR}/core/src/application/target/identity_log_feed_application.cpp)
set_property(TARGET kiarabot_identity_log_feed_application PROPERTY KIARABOT_LAYER INNER)
target_include_directories(kiarabot_identity_log_feed_application PUBLIC
  ${CMAKE_SOURCE_DIR}/core/include
  PRIVATE ${CMAKE_SOURCE_DIR}/core/src/application/target)
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
  target_link_libraries(kiarabot_postgres PRIVATE kiarabot_observation_feed_identity)
  target_link_libraries(kiarabot_postgres PRIVATE kiarabot_observation_feed_identity)
  if(TARGET kiarabot-identity)
    target_link_libraries(kiarabot-identity PRIVATE kiarabot_identity_log_feed_application
      kiarabot_observation_feed_rpc)
  endif()
  if(TARGET kiarabot-scheduler)
    target_link_libraries(kiarabot-scheduler PRIVATE kiarabot_observation_feed_application
      kiarabot_observation_feed_rpc)
  endif()
  if(TARGET kiarabot-runtime)
    target_link_libraries(kiarabot-runtime PRIVATE kiarabot_observation_feed_application
      kiarabot_observation_feed_rpc)
  endif()
  # Test and auxiliary binaries that link an owner RPC library now reference the feed encode
  # helpers; link the support library where the target exists.
  foreach(feed_consumer kiarabot_identity_rpc_test kiarabot_scheduler_rpc_test
                        kiarabot_model_bridge_test kiarabot_model_rpc_test kiarabot-runtime-p13g)
    if(TARGET ${feed_consumer})
      target_link_libraries(${feed_consumer} PRIVATE kiarabot_observation_feed_rpc)
    endif()
  endforeach()
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
