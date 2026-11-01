# New Core targets use only core/include; old protocol/snapshot code is reference-only.
add_subdirectory(reference "${CMAKE_BINARY_DIR}/reference/core")

# P01 pure internal-model assertions are not reference business tests.
foreach(COMPONENT foundation invariants)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME architecture_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(architecture_${COMPONENT} PROPERTIES LABELS "p01;unit" TIMEOUT 10)
endforeach()

add_executable(kiarabot_persistence_contract_test tests/architecture/persistence_contract_test.cpp)
target_include_directories(kiarabot_persistence_contract_test PRIVATE include)
target_compile_features(kiarabot_persistence_contract_test PRIVATE cxx_std_20)
target_compile_options(kiarabot_persistence_contract_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_test(NAME p03_persistence_contract COMMAND kiarabot_persistence_contract_test)
set_tests_properties(p03_persistence_contract PROPERTIES LABELS "p03;unit" TIMEOUT 10)
