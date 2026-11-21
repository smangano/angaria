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

add_library(angaria STATIC
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
  src/runtime_session.cpp
  src/task_runtime.cpp
  src/task_claim_redo.cpp
)
target_include_directories(angaria PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(angaria PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(angaria PUBLIC cxx_std_20)
target_link_libraries(angaria PUBLIC Boost::headers Threads::Threads)

add_executable(angaria_queue_demo examples/queue_demo.cpp)
target_link_libraries(angaria_queue_demo PRIVATE angaria)

# Compile the future runtime API example as a declaration contract.
add_library(angaria_api_contract OBJECT examples/trading_api.cpp)
target_link_libraries(angaria_api_contract PRIVATE angaria)

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  include(FetchContent)
  FetchContent_Declare(
    cpptoml
    GIT_REPOSITORY https://github.com/skystrife/cpptoml.git
    GIT_TAG fededad7169e538ca47e11a9ee9251bc361a9a65
    SOURCE_SUBDIR include
  )
  FetchContent_MakeAvailable(cpptoml)

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

  add_executable(angaria_tests
    test/test.cpp
    test/test_field.cpp
    test/test_session.cpp
    test/test_configuration.cpp
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
    ANGARIA_TEST_VERSION="${PROJECT_VERSION}"
  )
  target_include_directories(angaria_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_tests PRIVATE angaria GTest::gtest)
  add_dependencies(angaria_tests angaria_native_atomic_worker angaria_native_controller_worker angaria_native_journal_worker angaria_native_session_worker)

  include(GoogleTest)
  gtest_discover_tests(angaria_tests PROPERTIES TIMEOUT 10)
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
