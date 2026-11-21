cmake_minimum_required(VERSION 3.20)

# Exercise the earlier v0 queue executable as a user would from the command line.
# This prototype's bounded queue and log are separate from format-2 Queue policies;
# test_runtime_session.cpp demonstrates the implemented public Session API.
string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 session_name)
set(session "${CMAKE_CURRENT_BINARY_DIR}/${session_name}")
set(log "${session}/session.ang")
set(empty_input "${CMAKE_CURRENT_BINARY_DIR}/empty-${session_name}.txt")
set(ack_input "${CMAKE_CURRENT_BINARY_DIR}/ack-${session_name}.txt")

function(check_demo expected_status expected_output command)
  set(path "${session}")
  set(input_file "${empty_input}")
  if(command STREQUAL "ack_receive")
    set(command receive)
    set(input_file "${ack_input}")
  endif()
  if(command STREQUAL "inspect")
    set(path "${log}")
  endif()
  execute_process(
    COMMAND "${DEMO}" "${command}" "${path}" ${ARGN}
    INPUT_FILE "${input_file}"
    TIMEOUT 5
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX REPLACE "timestamp_ns=-?[0-9]+" "timestamp_ns=TIME" output "${output}")
  if(NOT "${status}" STREQUAL "${expected_status}" OR NOT "${output}${error}" STREQUAL "${expected_output}")
    if(NOT command STREQUAL "create")
      file(REMOVE_RECURSE "${session}")
    endif()
    file(REMOVE "${empty_input}" "${ack_input}")
    message(FATAL_ERROR "${command} ${ARGN} returned ${status}, stdout='${output}', stderr='${error}'")
  endif()
endfunction()

# Every child receives explicit input, including EOF for an unacknowledged receive.
file(WRITE "${empty_input}" "")
file(WRITE "${ack_input}" "ack\n")

# A 50-byte header and two 30-byte records exactly fill this log.
check_demo(0 "" create 110)
check_demo(1 "angaria_queue_demo: value must be a signed 32-bit integer" send 2147483648)
check_demo(1 "angaria_queue_demo: queue is empty" receive)
check_demo(0 "" send 42)
check_demo(0 "" send -7)
# A receive without acknowledgment leaves the same delivery pending and the queue
# full. Acknowledging frees a queue slot, but the already-full append log still
# rejects another send; neither kind of rejection should rewrite retained history.
file(SHA256 "${log}" before_rejection)
check_demo(3 "angaria_queue_demo: queue is full" send 9)
check_demo(0 "Reading sequence=1 value=42" receive)
check_demo(3 "angaria_queue_demo: queue is full" send 9)
check_demo(0 "Reading sequence=1 value=42" ack_receive)
check_demo(4 "angaria_queue_demo: session log is full" send 9)
check_demo(0 "Reading sequence=2 value=-7" ack_receive)
check_demo(1 "angaria_queue_demo: queue is empty" receive)
file(SHA256 "${log}" after_rejection)
if(NOT before_rejection STREQUAL after_rejection)
  file(REMOVE_RECURSE "${session}")
  file(REMOVE "${empty_input}" "${ack_input}")
  message(FATAL_ERROR "A rejection or receive changed the session log")
endif()
# Removing the live queue does not erase its committed log: inspect still decodes
# both messages from stored schema information after the endpoints have closed.
check_demo(0 "" remove)
check_demo(0 "queue=${session_name} sequence=1 timestamp_ns=TIME type=Reading value=42\nqueue=${session_name} sequence=2 timestamp_ns=TIME type=Reading value=-7" inspect)
file(REMOVE_RECURSE "${session}")
file(REMOVE "${empty_input}" "${ack_input}")
