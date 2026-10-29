add_executable(kiarabot-core src/main.cpp)
target_compile_features(kiarabot-core PRIVATE cxx_std_20)
target_include_directories(kiarabot-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(kiarabot-core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(kiarabot-core-smoke tests/core_smoke.cpp)
target_include_directories(kiarabot-core-smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME core_smoke COMMAND kiarabot-core-smoke)
