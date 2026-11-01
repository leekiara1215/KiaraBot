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

foreach(COMPONENT persistence_contract persistence_rules time)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p03_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p03_${COMPONENT} PROPERTIES LABELS "p03;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT identity_contract identity_application_contract)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p04_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p04_${COMPONENT} PROPERTIES LABELS "p04;unit" TIMEOUT 10)
endforeach()

foreach(COMPONENT identity_rules identity_grant_projection)
  add_executable(kiarabot_${COMPONENT}_test tests/architecture/${COMPONENT}_test.cpp)
  target_include_directories(kiarabot_${COMPONENT}_test PRIVATE include)
  target_compile_features(kiarabot_${COMPONENT}_test PRIVATE cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
  add_test(NAME p04_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p04_${COMPONENT} PROPERTIES LABELS "p04;unit;security" TIMEOUT 10)
endforeach()

# Identity administrative/delegation use cases stay pure and independently linkable.
foreach(COMPONENT identity_admin identity_delegation)
  add_library(kiarabot_${COMPONENT}_application STATIC
    src/application/target/${COMPONENT}_application.cpp)
  target_include_directories(kiarabot_${COMPONENT}_application PUBLIC include)
  target_compile_features(kiarabot_${COMPONENT}_application PUBLIC cxx_std_20)
  target_compile_options(kiarabot_${COMPONENT}_application PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
endforeach()
add_executable(kiarabot_identity_admin_application_test
  tests/architecture/identity_admin_application_test.cpp)
target_link_libraries(kiarabot_identity_admin_application_test PRIVATE
  kiarabot_identity_admin_application kiarabot_identity_delegation_application)
target_compile_options(kiarabot_identity_admin_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_test(NAME p04_identity_admin_application COMMAND kiarabot_identity_admin_application_test)
set_tests_properties(p04_identity_admin_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 10)

add_library(kiarabot_identity_security_application STATIC
  src/application/target/identity_security_application.cpp)
target_include_directories(kiarabot_identity_security_application PUBLIC include)
target_compile_features(kiarabot_identity_security_application PUBLIC cxx_std_20)
target_compile_options(kiarabot_identity_security_application PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_executable(kiarabot_identity_security_application_test
  tests/architecture/identity_security_application_test.cpp)
target_link_libraries(kiarabot_identity_security_application_test PRIVATE
  kiarabot_identity_security_application)
target_compile_options(kiarabot_identity_security_application_test PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>)
add_test(NAME p04_identity_security_application COMMAND kiarabot_identity_security_application_test)
set_tests_properties(p04_identity_security_application PROPERTIES
  LABELS "p04;unit;security" TIMEOUT 30)
