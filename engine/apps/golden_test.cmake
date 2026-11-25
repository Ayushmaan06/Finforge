# Runs finforge-sim on INPUT and compares stdout with EXPECTED, byte for byte.
execute_process(COMMAND ${SIM} ${INPUT} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "finforge-sim exited with ${rc}")
endif()
file(READ ${EXPECTED} expected)
if(NOT out STREQUAL expected)
  file(WRITE ${ACTUAL} "${out}")
  message(FATAL_ERROR "output differs from ${EXPECTED}; actual written to ${ACTUAL}\n"
                      "diff: diff -u ${EXPECTED} ${ACTUAL}")
endif()
