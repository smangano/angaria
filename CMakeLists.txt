cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANGARIA_VERSION)
string(STRIP "${ANGARIA_VERSION}" ANGARIA_VERSION)
if(NOT ANGARIA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must use major.minor.build format")
endif()
project(Angaria VERSION "${ANGARIA_VERSION}" LANGUAGES CXX)
configure_file(VERSION VERSION COPYONLY)
configure_file(src/build_version.h.in generated/build_version.h @ONLY)
message(STATUS "Angaria version ${PROJECT_VERSION}")

find_package(Boost 1.70 CONFIG REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  cpptoml
  GIT_REPOSITORY https://github.com/skystrife/cpptoml.git
  GIT_TAG fededad7169e538ca47e11a9ee9251bc361a9a65
  SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(cpptoml)

add_library(angaria STATIC
  src/capture.cpp
  src/inspection.cpp
  src/message_record.cpp
  src/message.cpp
  src/publication_metadata.cpp
  src/publication_runtime.cpp
  src/mapped_payload.cpp
  src/delivery_state.cpp
  src/delivery_runtime.cpp
  src/delivery_progress.cpp
  src/payload.cpp
  src/configuration.cpp
  src/configuration_runtime.cpp
  src/session.cpp
  src/session_log.cpp
  src/role_lock.cpp
  src/mapped_superblock.cpp
  src/native_file.cpp
  src/mapped_region.cpp
  src/native_atomic.cpp
  src/session_controller.cpp
  src/journal_format.cpp
  src/journal_validation.cpp
  src/mutation_journal.cpp
  src/session_metadata.cpp
  src/runtime_state.cpp
  src/registry_snapshot.cpp
  src/runtime_session.cpp
  src/task_runtime.cpp
  src/task_claim_redo.cpp
  src/task_registry.cpp
  src/schema_metadata.cpp
  src/schema_runtime.cpp
  src/schema_registry.cpp
  src/topology_metadata.cpp
  src/topology_runtime.cpp
  src/topology_registry.cpp
  src/topology_policy.cpp
  src/topology_append.cpp
  src/topology_mutations.cpp
  src/endpoint_runtime.cpp
)
target_include_directories(angaria PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(angaria PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(angaria SYSTEM PRIVATE "${cpptoml_SOURCE_DIR}/include")
target_compile_features(angaria PUBLIC cxx_std_20)
target_link_libraries(angaria PUBLIC Boost::headers Threads::Threads)

add_executable(angaria_queue_demo examples/queue_demo.cpp)
target_link_libraries(angaria_queue_demo PRIVATE angaria)

add_executable(angaria_inspect tools/inspect.cpp)
target_link_libraries(angaria_inspect PRIVATE angaria)

# Compile the future runtime API example as a declaration contract.
add_library(angaria_api_contract OBJECT examples/trading_api.cpp)
target_link_libraries(angaria_api_contract PRIVATE angaria)

# Review-only declarations have no runtime definitions. Compile real caller
# workflows without linking stubs or exposing them as runnable examples.
add_library(angaria_public_api_review OBJECT examples/api_review.cpp)
target_include_directories(angaria_public_api_review PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(angaria_public_api_review PRIVATE cxx_std_20)

# Primer programs use implemented public APIs and remain runnable without tests.
foreach(lesson IN ITEMS 01_session 02_tasks 03_schemas 04_connections 05_fanout 06_processes 07_configuration 08_registry 09_payload 10_logical_fields 11_message_record 12_capture 13_process_exchange 14_direct_send)
  add_executable(angaria_primer_${lesson} examples/primer/${lesson}.cpp)
  target_link_libraries(angaria_primer_${lesson} PRIVATE angaria)
endforeach()

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

  add_executable(angaria_native_atomic_worker test/native_atomic_worker.cpp)
  target_include_directories(angaria_native_atomic_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_atomic_worker PRIVATE angaria)

  add_executable(angaria_native_controller_worker test/native_controller_worker.cpp)
  target_include_directories(angaria_native_controller_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_controller_worker PRIVATE angaria)

  add_executable(angaria_native_journal_worker test/native_journal_worker.cpp)
  target_include_directories(angaria_native_journal_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_journal_worker PRIVATE angaria)

  add_executable(angaria_native_session_worker test/native_session_worker.cpp)
  target_include_directories(angaria_native_session_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_session_worker PRIVATE angaria)

  add_executable(angaria_native_delivery_progress_worker test/native_delivery_progress_worker.cpp)
  target_include_directories(angaria_native_delivery_progress_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_delivery_progress_worker PRIVATE angaria)

  add_executable(angaria_native_publication_worker test/native_publication_worker.cpp)
  target_include_directories(angaria_native_publication_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_publication_worker PRIVATE angaria)

  add_executable(angaria_native_registration_worker test/native_registration_worker.cpp)
  target_include_directories(angaria_native_registration_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_registration_worker PRIVATE angaria)

  add_executable(angaria_native_schema_worker test/native_schema_worker.cpp)
  target_include_directories(angaria_native_schema_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_schema_worker PRIVATE angaria)

  add_executable(angaria_native_topology_worker test/native_topology_worker.cpp)
  target_include_directories(angaria_native_topology_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_topology_worker PRIVATE angaria)

  add_executable(angaria_native_activation_worker test/native_activation_worker.cpp)
  target_include_directories(angaria_native_activation_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_activation_worker PRIVATE angaria)

  add_executable(angaria_native_registry_worker test/native_registry_worker.cpp)
  target_include_directories(angaria_native_registry_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_registry_worker PRIVATE angaria)

  add_executable(angaria_capture_diagnostic_worker test/capture_diagnostic_worker.cpp)
  target_link_libraries(angaria_capture_diagnostic_worker PRIVATE angaria)

  add_executable(angaria_tests
    test/test.cpp
    test/test_shared_helpers.cpp
    test/test_append_layout.cpp
    test/test_field.cpp
    test/test_payload.cpp
    test/test_message_record.cpp
    test/test_message.cpp
    test/test_publication_metadata.cpp
    test/test_publication_api.cpp
    test/test_publication_runtime.cpp
    test/test_publication_recovery.cpp
    test/test_mapped_payload.cpp
    test/test_payload_rollback.cpp
    test/test_delivery_state.cpp
    test/test_delivery_framing.cpp
    test/test_delivery_compatibility.cpp
    test/test_delivery_validation.cpp
    test/test_delivery_progress.cpp
    test/test_delivery_claim_cleanup.cpp
    test/test_delivery_progress_recovery.cpp
    test/test_capture.cpp
    test/test_inspection.cpp
    test/test_session.cpp
    test/test_configuration.cpp
    test/test_file_configuration.cpp
    test/test_registry_snapshot.cpp
    test/test_registry_qualification.cpp
    test/test_task_catalog_validation.cpp
    test/test_identities.cpp
    test/test_api_contract.cpp
    test/test_mapped_format.cpp
    test/test_mapped_superblock.cpp
    test/test_native_file.cpp
    test/test_native_atomic.cpp
    test/test_session_controller.cpp
    test/test_journal_format.cpp
    test/test_journal_validation.cpp
    test/test_mutation_journal.cpp
    test/test_journal_bulk_redo.cpp
    test/test_session_metadata.cpp
    test/test_runtime_session.cpp
    test/test_runtime_access.cpp
    test/test_task_registration.cpp
    test/test_task_registration_recovery.cpp
    test/test_schema_metadata.cpp
    test/test_schema_registry.cpp
    test/test_schema_recovery.cpp
    test/test_topology_metadata.cpp
    test/test_topology_registry.cpp
    test/test_topology_recovery.cpp
    test/test_topology_activation.cpp
    test/test_endpoint_runtime.cpp
    test/test_activation_recovery.cpp
  )
  target_include_directories(angaria_tests SYSTEM PRIVATE "${cpptoml_SOURCE_DIR}/include")
  target_compile_definitions(
    angaria_tests PRIVATE
    ANGARIA_CONFIGURATION_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/examples/trading.toml"
    ANGARIA_TEST_PYTHON="${Python3_EXECUTABLE}"
    ANGARIA_NATIVE_ATOMIC_WORKER="$<TARGET_FILE:angaria_native_atomic_worker>"
    ANGARIA_NATIVE_CONTROLLER_WORKER="$<TARGET_FILE:angaria_native_controller_worker>"
    ANGARIA_NATIVE_JOURNAL_WORKER="$<TARGET_FILE:angaria_native_journal_worker>"
    ANGARIA_NATIVE_SESSION_WORKER="$<TARGET_FILE:angaria_native_session_worker>"
    ANGARIA_NATIVE_DELIVERY_PROGRESS_WORKER="$<TARGET_FILE:angaria_native_delivery_progress_worker>"
    ANGARIA_NATIVE_PUBLICATION_WORKER="$<TARGET_FILE:angaria_native_publication_worker>"
    ANGARIA_NATIVE_REGISTRATION_WORKER="$<TARGET_FILE:angaria_native_registration_worker>"
    ANGARIA_NATIVE_SCHEMA_WORKER="$<TARGET_FILE:angaria_native_schema_worker>"
    ANGARIA_NATIVE_TOPOLOGY_WORKER="$<TARGET_FILE:angaria_native_topology_worker>"
    ANGARIA_NATIVE_ACTIVATION_WORKER="$<TARGET_FILE:angaria_native_activation_worker>"
    ANGARIA_NATIVE_REGISTRY_WORKER="$<TARGET_FILE:angaria_native_registry_worker>"
    ANGARIA_TEST_VERSION="${PROJECT_VERSION}"
  )
  target_include_directories(angaria_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_tests PRIVATE angaria GTest::gtest)
  add_dependencies(angaria_tests angaria_native_atomic_worker angaria_native_controller_worker angaria_native_journal_worker angaria_native_session_worker angaria_native_delivery_progress_worker angaria_native_publication_worker angaria_native_registration_worker angaria_native_schema_worker angaria_native_topology_worker angaria_native_activation_worker angaria_native_registry_worker)

  include(GoogleTest)
  gtest_discover_tests(angaria_tests NO_PRETTY_VALUES PROPERTIES TIMEOUT 10)
  add_test(
    NAME queue_demo
    COMMAND "${CMAKE_COMMAND}" "-DDEMO=$<TARGET_FILE:angaria_queue_demo>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test/queue_demo.cmake"
  )
  set_tests_properties(queue_demo PROPERTIES TIMEOUT 15)
  add_test(
    NAME restart_demo
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/restart_demo.py"
            "$<TARGET_FILE:angaria_queue_demo>"
  )
  set_tests_properties(restart_demo PROPERTIES TIMEOUT 30)
  add_test(
    NAME primer_examples
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/primer_examples.py"
            "$<TARGET_FILE:angaria_primer_01_session>"
            "$<TARGET_FILE:angaria_primer_02_tasks>"
            "$<TARGET_FILE:angaria_primer_03_schemas>"
            "$<TARGET_FILE:angaria_primer_04_connections>"
            "$<TARGET_FILE:angaria_primer_05_fanout>"
            "$<TARGET_FILE:angaria_primer_06_processes>"
            "$<TARGET_FILE:angaria_primer_07_configuration>"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/primer/configuration.toml"
            "$<TARGET_FILE:angaria_primer_14_direct_send>"
  )
  set_tests_properties(primer_examples PROPERTIES TIMEOUT 45)
  add_test(
    NAME registry_walkthrough
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/registry_walkthrough.py"
            "$<TARGET_FILE:angaria_primer_08_registry>"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/primer/configuration.toml"
  )
  set_tests_properties(registry_walkthrough PROPERTIES TIMEOUT 45)
  add_test(
    NAME payload_example
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/payload_example.py"
            "$<TARGET_FILE:angaria_primer_09_payload>"
  )
  set_tests_properties(payload_example PROPERTIES TIMEOUT 20)
  add_test(
    NAME logical_fields_example
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/logical_fields_example.py"
            "$<TARGET_FILE:angaria_primer_10_logical_fields>"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/primer/logical-fields.toml"
  )
  set_tests_properties(logical_fields_example PROPERTIES TIMEOUT 20)
  add_test(
    NAME message_record_example
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/message_record_example.py"
            "$<TARGET_FILE:angaria_primer_11_message_record>"
  )
  set_tests_properties(message_record_example PROPERTIES TIMEOUT 20)
  add_test(
    NAME capture_example
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/capture_example.py"
            "$<TARGET_FILE:angaria_primer_12_capture>" "$<TARGET_FILE:angaria_inspect>"
            "$<TARGET_FILE:angaria_capture_diagnostic_worker>"
  )
  set_tests_properties(capture_example PROPERTIES TIMEOUT 60)
  add_test(
    NAME process_exchange
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/process_exchange.py"
            "$<TARGET_FILE:angaria_primer_13_process_exchange>" "$<TARGET_FILE:angaria_inspect>"
  )
  set_tests_properties(process_exchange PROPERTIES TIMEOUT 60)
  add_test(
    NAME protocol_model
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/protocol_model.py"
  )
  set_tests_properties(protocol_model PROPERTIES TIMEOUT 10)

  # Keep before-main native initialization checks out of GTest discovery.
  add_executable(angaria_native_storage_startup test/native_storage_startup.cpp)
  target_include_directories(angaria_native_storage_startup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_storage_startup PRIVATE angaria)
  add_test(NAME native_storage_startup COMMAND "$<TARGET_FILE:angaria_native_storage_startup>")
  set_tests_properties(native_storage_startup PROPERTIES TIMEOUT 10)

  # Retain owners populated in main until normal static destruction.
  add_executable(angaria_native_controller_shutdown test/native_controller_shutdown.cpp)
  target_include_directories(angaria_native_controller_shutdown PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_native_controller_shutdown PRIVATE angaria)
  add_test(NAME native_controller_shutdown COMMAND "$<TARGET_FILE:angaria_native_controller_shutdown>")
  set_tests_properties(native_controller_shutdown PROPERTIES TIMEOUT 10)
endif()
