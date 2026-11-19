cmake_minimum_required(VERSION 3.20)

project(Angaria LANGUAGES CXX)

find_package(Boost 1.70 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(angaria STATIC src/session.cpp src/session_log.cpp src/role_lock.cpp)
target_include_directories(angaria PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(angaria PUBLIC cxx_std_20)
target_link_libraries(angaria PUBLIC Boost::headers Threads::Threads)

add_executable(angaria_queue_demo examples/queue_demo.cpp)
target_link_libraries(angaria_queue_demo PRIVATE angaria)

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

  add_executable(angaria_tests test/test.cpp test/test_field.cpp test/test_session.cpp test/test_configuration.cpp)
  target_include_directories(angaria_tests SYSTEM PRIVATE "${cpptoml_SOURCE_DIR}/include")
  target_compile_definitions(
    angaria_tests PRIVATE
    ANGARIA_CONFIGURATION_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/examples/trading.toml"
  )
  target_include_directories(angaria_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(angaria_tests PRIVATE angaria GTest::gtest)

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
endif()
