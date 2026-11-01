# Identity cryptography is an outer adapter; inner ports remain library-independent.
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(nlohmann_json 3.11.3 EXACT CONFIG REQUIRED)
add_library(kiarabot_identity_crypto STATIC src/password_crypto.cpp src/token_crypto.cpp
  src/receipt_crypto.cpp src/peer_certificate.cpp src/delegation_crypto.cpp)
target_include_directories(kiarabot_identity_crypto PUBLIC
  include ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(kiarabot_identity_crypto PRIVATE unofficial-sodium::sodium OpenSSL::Crypto
  jwt-cpp::jwt-cpp nlohmann_json::nlohmann_json)
kiarabot_platform_warnings(kiarabot_identity_crypto)

foreach(COMPONENT password_crypto token_crypto receipt_crypto peer_certificate delegation_crypto)
  add_executable(kiarabot_${COMPONENT}_test tests/${COMPONENT}_test.cpp)
  target_link_libraries(kiarabot_${COMPONENT}_test PRIVATE kiarabot_identity_crypto)
  kiarabot_platform_warnings(kiarabot_${COMPONENT}_test)
  # These source-owned checks use assert; retain them in every build configuration.
  target_compile_options(kiarabot_${COMPONENT}_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
  add_test(NAME p04_${COMPONENT} COMMAND kiarabot_${COMPONENT}_test)
  set_tests_properties(p04_${COMPONENT} PROPERTIES LABELS "p04;unit;security" TIMEOUT 30)
endforeach()

# Receipt tamper checks use sodium directly; keep this dependency private to the test.
target_link_libraries(kiarabot_receipt_crypto_test PRIVATE unofficial-sodium::sodium)

# This test generates real X509 evidence through the same locked OpenSSL package.
target_link_libraries(kiarabot_peer_certificate_test PRIVATE OpenSSL::Crypto)

# The JWT test signs real keys and uses official nlohmann traits directly.
target_link_libraries(kiarabot_delegation_crypto_test PRIVATE
  jwt-cpp::jwt-cpp nlohmann_json::nlohmann_json OpenSSL::Crypto)
