# JSON is pinned by content hash. It is downloaded only into the ignored build tree.
set(KIARABOT_JSON_INCLUDE "${CMAKE_BINARY_DIR}/dependencies/include")
file(MAKE_DIRECTORY "${KIARABOT_JSON_INCLUDE}/nlohmann")
file(DOWNLOAD
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  "${KIARABOT_JSON_INCLUDE}/nlohmann/json.hpp"
  EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
  TLS_VERIFY ON)
add_library(kiarabot_control_json INTERFACE)
target_include_directories(kiarabot_control_json SYSTEM INTERFACE "${KIARABOT_JSON_INCLUDE}")
find_package(Boost 1.86 CONFIG REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)
target_sources(kiarabot_bridge PRIVATE ${CMAKE_SOURCE_DIR}/core/src/control_service.cpp
  ${CMAKE_SOURCE_DIR}/core/src/control_crypto.cpp
  ${CMAKE_SOURCE_DIR}/core/src/plugin_config_delivery.cpp)
target_include_directories(kiarabot_bridge SYSTEM PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(kiarabot_bridge PUBLIC kiarabot_control_json PRIVATE ${SODIUM_LIBRARY})
add_library(kiarabot_control STATIC src/http_mapper.cpp src/http_server.cpp src/configuration.cpp src/event_stream.cpp)
target_include_directories(kiarabot_control PUBLIC include)
target_link_libraries(kiarabot_control PUBLIC kiarabot_bridge PRIVATE Boost::headers)
target_compile_options(kiarabot_control PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot-control-credentials src/credentials.cpp)
target_link_libraries(kiarabot-control-credentials PRIVATE kiarabot_bridge)
target_link_libraries(kiarabot-core PRIVATE kiarabot_control)
add_executable(kiarabot-event-stream-test tests/event_stream_test.cpp)
target_link_libraries(kiarabot-event-stream-test PRIVATE kiarabot_control)
add_test(NAME control_event_stream COMMAND kiarabot-event-stream-test)
if(KIARABOT_ENABLE_INFRASTRUCTURE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/.venv/bin/python")
  add_test(NAME control_http_websocket_acceptance
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/.venv/bin/python" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  set_tests_properties(control_http_websocket_acceptance PROPERTIES
    TIMEOUT 90 ENVIRONMENT "KIARABOT_ACCEPTANCE_BUILD=${CMAKE_BINARY_DIR}")
endif()
add_executable(kiarabot-control-mock tests/mock_server.cpp)
target_link_libraries(kiarabot-control-mock PRIVATE kiarabot_control)
