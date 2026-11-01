cmake_minimum_required(VERSION 3.24)
project(kiarabot_dependency_smoke LANGUAGES CXX)
find_package(libpqxx CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(nlohmann_json 3.11.3 EXACT CONFIG REQUIRED)
find_package(OpenSSL 3.5.0 EXACT REQUIRED COMPONENTS SSL Crypto)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)
add_executable(kiarabot_dependency_smoke smoke.cpp)
target_compile_features(kiarabot_dependency_smoke PRIVATE cxx_std_20)
target_compile_options(kiarabot_dependency_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(kiarabot_dependency_smoke SYSTEM PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(kiarabot_dependency_smoke PRIVATE libpqxx::pqxx jwt-cpp::jwt-cpp nlohmann_json::nlohmann_json
  OpenSSL::SSL OpenSSL::Crypto CURL::libcurl protobuf::libprotobuf
  gRPC::grpc++ Boost::headers ${SODIUM_LIBRARY})

if(TARGET SQLite3::SQLite3)
  target_link_libraries(kiarabot_dependency_smoke PRIVATE SQLite3::SQLite3)
else()
  target_link_libraries(kiarabot_dependency_smoke PRIVATE SQLite::SQLite3)
endif()

set_target_properties(kiarabot_dependency_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(KIARABOT_TEST_PG_BIN "${CMAKE_SOURCE_DIR}/build/pg-16.4-install/bin" CACHE PATH
  "Explicit disposable PostgreSQL 16.4 tools for the P00 probe")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_test(NAME p00_dependency_smoke COMMAND "${KIARABOT_TARGET_PYTHON}"
    "${CMAKE_SOURCE_DIR}/scripts/quality/run-dependency-smoke.py"
    --pg-bin "${KIARABOT_TEST_PG_BIN}" --openssl "${KIARABOT_TEST_OPENSSL}"
    --smoke $<TARGET_FILE:kiarabot_dependency_smoke>)
  set_tests_properties(p00_dependency_smoke PROPERTIES LABELS "p00;integration" TIMEOUT 90)
endif()
