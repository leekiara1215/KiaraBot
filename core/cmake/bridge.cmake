# Core owns its application and runtime sources and their build dependencies.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)
set(KIARABOT_JSON_INCLUDE "${CMAKE_BINARY_DIR}/dependencies/include")
file(MAKE_DIRECTORY "${KIARABOT_JSON_INCLUDE}/nlohmann")
file(DOWNLOAD
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  "${KIARABOT_JSON_INCLUDE}/nlohmann/json.hpp"
  EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
  TLS_VERIFY ON)
add_library(kiarabot_core_json INTERFACE)
target_include_directories(kiarabot_core_json SYSTEM INTERFACE "${KIARABOT_JSON_INCLUDE}")
add_library(kiarabot_bridge
  ${CMAKE_CURRENT_LIST_DIR}/../src/runtime/bridge_server.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/runtime/request_digest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/adapter_ingress.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/plugin_hub.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/action_quota.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/action_router.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/host_dispatcher.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/task_scheduler.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/control_application.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/control_service.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/control_crypto.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/plugin_config_delivery.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/application/runtime_maintenance.cpp)
target_include_directories(kiarabot_bridge PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include)
target_include_directories(kiarabot_bridge SYSTEM PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(kiarabot_bridge PUBLIC kiarabot_proto kiarabot_core_json OpenSSL::Crypto
  PRIVATE ${SODIUM_LIBRARY})
