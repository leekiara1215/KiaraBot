# Core owns its application and runtime sources and their build dependencies.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)
find_package(nlohmann_json 3.11.3 CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
  add_library(kiarabot_core_json INTERFACE)
  target_link_libraries(kiarabot_core_json INTERFACE nlohmann_json::nlohmann_json)
else()
  set(KIARABOT_JSON_INCLUDE "${CMAKE_BINARY_DIR}/dependencies/include")
  set(KIARABOT_JSON_HEADER "${KIARABOT_JSON_INCLUDE}/nlohmann/json.hpp")
  file(MAKE_DIRECTORY "${KIARABOT_JSON_INCLUDE}/nlohmann")
  set(KIARABOT_JSON_HASH "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6")
  if(EXISTS "${KIARABOT_JSON_HEADER}")
    file(SHA256 "${KIARABOT_JSON_HEADER}" ACTUAL_JSON_HASH)
    if(NOT ACTUAL_JSON_HASH STREQUAL KIARABOT_JSON_HASH)
      message(FATAL_ERROR "Cached JSON header checksum mismatch")
    endif()
  else()
    file(DOWNLOAD
      https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
      "${KIARABOT_JSON_HEADER}" EXPECTED_HASH SHA256=${KIARABOT_JSON_HASH} TLS_VERIFY ON)
  endif()
  add_library(kiarabot_core_json INTERFACE)
  target_include_directories(kiarabot_core_json SYSTEM INTERFACE "${KIARABOT_JSON_INCLUDE}")
endif()
add_library(kiarabot_bridge
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/runtime/bridge_server.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/runtime/request_digest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/adapter_ingress.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/plugin_hub.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/action_quota.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/action_router.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/host_dispatcher.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/task_scheduler.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/control_application.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/control_service.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/control_crypto.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/plugin_config_delivery.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../reference/src/application/runtime_maintenance.cpp)
target_include_directories(kiarabot_bridge PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../reference/include)
target_include_directories(kiarabot_bridge SYSTEM PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(kiarabot_bridge PUBLIC kiarabot_proto kiarabot_core_json OpenSSL::Crypto
  PRIVATE ${SODIUM_LIBRARY})
