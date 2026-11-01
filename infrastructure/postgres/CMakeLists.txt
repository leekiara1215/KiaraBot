# PostgreSQL is an outer adapter; no database dependency reaches application libraries.
find_package(libpqxx 7.10.0 EXACT CONFIG REQUIRED)
add_library(kiarabot_postgres STATIC
  src/database.cpp src/common_work.cpp src/record_codec.cpp src/factories.cpp)
target_include_directories(kiarabot_postgres PUBLIC
  include ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_postgres PRIVATE libpqxx::pqxx Threads::Threads)
kiarabot_platform_warnings(kiarabot_postgres)

add_executable(kiarabot-pg-initialize migrate/initialize_main.cpp)
target_link_libraries(kiarabot-pg-initialize PRIVATE kiarabot_postgres)
kiarabot_platform_warnings(kiarabot-pg-initialize)

if(UNIX)
  add_executable(kiarabot_postgres_database_test
    tests/database_test.cpp tests/common_work_checks.cpp tests/typed_work_checks.cpp)
  target_link_libraries(kiarabot_postgres_database_test PRIVATE kiarabot_postgres libpqxx::pqxx)
  kiarabot_platform_warnings(kiarabot_postgres_database_test)
  # One process owns setup, faults and cleanup; parallel runs have private clusters.
  add_test(NAME p03_postgres_database COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_database_test.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --test $<TARGET_FILE:kiarabot_postgres_database_test>
    --initializer $<TARGET_FILE:kiarabot-pg-initialize>)
  set_tests_properties(p03_postgres_database PROPERTIES LABELS "p03;integration;fault" TIMEOUT 120)
endif()
