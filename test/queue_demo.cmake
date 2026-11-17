cmake_minimum_required(VERSION 3.20)

string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 session)

function(check_demo expected_status expected_output command)
  execute_process(
    COMMAND "${DEMO}" "${command}" "${session}" ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  if(NOT "${status}" STREQUAL "${expected_status}" OR NOT "${output}${error}" STREQUAL "${expected_output}")
    if(NOT command STREQUAL "create" AND NOT command STREQUAL "remove")
      execute_process(COMMAND "${DEMO}" remove "${session}" ERROR_QUIET)
    endif()
    message(FATAL_ERROR "${command} ${ARGN} returned ${status}, stdout='${output}', stderr='${error}'")
  endif()
endfunction()

check_demo(0 "" create)
check_demo(1 "angaria_queue_demo: value must be a signed 32-bit integer" send 2147483648)
check_demo(1 "angaria_queue_demo: queue is empty" receive)
check_demo(0 "" send 42)
check_demo(0 "" send -7)
check_demo(1 "angaria_queue_demo: queue is full" send 9)
check_demo(0 "Reading value=42" receive)
check_demo(0 "Reading value=-7" receive)
check_demo(1 "angaria_queue_demo: queue is empty" receive)
check_demo(0 "" remove)
