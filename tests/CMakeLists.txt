# Test suite. Every test is a real executable driven by CTest.
# No test declares a timeout: a hanging test is a defect, not a missing watchdog.

set(TOS_TEST_SOURCES)
file(GLOB TOS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER TOS_TEST_SOURCES EXCLUDE REGEX "tos_test_support\.cpp$")

foreach(test_source ${TOS_TEST_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tos_test_support.cpp)
  target_link_libraries(${test_name} PRIVATE transport_offload_scheduler)
  target_compile_options(${test_name} PRIVATE ${TOS_WARNING_FLAGS})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
