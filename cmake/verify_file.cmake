if(NOT DEFINED VERIFY_FILE OR NOT EXISTS "${VERIFY_FILE}")
  message(FATAL_ERROR "Expected generated file does not exist: ${VERIFY_FILE}")
endif()
