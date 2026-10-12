# Verifies that the builder has supplied a legitimate copy of the game in src/rom.
set(SWBF2_XBE_SHA1 "35ac5bd9ad3879f4a9c5eb5b306163e7b67f8964")

function(swbf2_check_rom ROM_DIR)
  set(_help "Copy your own dump of Star Wars Battlefront II (Xbox) into ${ROM_DIR} (see ${ROM_DIR}/README.md).")
  if(NOT EXISTS "${ROM_DIR}/default.xbe")
    message(FATAL_ERROR "default.xbe not found in ${ROM_DIR}.\n${_help}")
  endif()
  if(NOT IS_DIRECTORY "${ROM_DIR}/Data/_LVL_XBOX")
    message(FATAL_ERROR "Data/_LVL_XBOX not found in ${ROM_DIR}.\n${_help}")
  endif()
  file(SHA1 "${ROM_DIR}/default.xbe" _sha)
  if(NOT _sha STREQUAL SWBF2_XBE_SHA1)
    message(FATAL_ERROR
      "default.xbe has SHA-1 ${_sha}, expected ${SWBF2_XBE_SHA1}.\n"
      "This port targets one specific retail build of the game.")
  endif()
  message(STATUS "Found SWBF2 game files in ${ROM_DIR}")
endfunction()
