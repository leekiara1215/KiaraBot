# Core owns the logging decorator; infrastructure supplies only its sink.
if(NOT TARGET kiarabot_core_log)
  add_library(kiarabot_core_log STATIC ${CMAKE_CURRENT_LIST_DIR}/../src/runtime/runtime_log.cpp)
  target_include_directories(kiarabot_core_log PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include)
  target_compile_features(kiarabot_core_log PUBLIC cxx_std_20)
  target_compile_options(kiarabot_core_log PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
endif()
