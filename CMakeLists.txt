cmake_minimum_required(VERSION 3.20)

project(Angaria LANGUAGES CXX)

find_package(Boost 1.70 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(angaria INTERFACE)
target_include_directories(angaria INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(angaria INTERFACE cxx_std_20)
target_link_libraries(angaria INTERFACE Boost::headers Threads::Threads)

add_executable(angaria_queue_demo examples/queue_demo.cpp)
target_link_libraries(angaria_queue_demo PRIVATE angaria)

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  add_executable(angaria_tests test/test.cpp test/test_field.cpp)
  target_link_libraries(angaria_tests PRIVATE angaria GTest::gtest)

  include(GoogleTest)
  gtest_discover_tests(angaria_tests)
  add_test(
    NAME queue_demo
    COMMAND "${CMAKE_COMMAND}" "-DDEMO=$<TARGET_FILE:angaria_queue_demo>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test/queue_demo.cmake"
  )
  set_tests_properties(queue_demo PROPERTIES TIMEOUT 15)
endif()
