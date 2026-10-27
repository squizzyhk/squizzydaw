# services/audiodriver-wasapi: WASAPI IAudioDriver (shared + exclusive, event driven); a null
# driver with no devices on non-Windows builds. Sample conversion helpers are header-only
# (include/sq/audiodriver/sample_convert.hpp) so the ASIO driver can share them.
sq_add_service(NAME audiodriver-wasapi VERSION 0.1.0
  SOURCES src/wasapi_module.cpp src/wasapi_driver.cpp src/wasapi_driver.hpp
  DESCRIPTION "Squizzy Studio WASAPI audio driver")
target_include_directories(svc_audiodriver-wasapi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
  target_link_libraries(svc_audiodriver-wasapi PRIVATE ole32 avrt uuid)
endif()

sq_add_test(NAME audiodriver_wasapi_convert SOURCES tests/test_convert.cpp)
target_include_directories(sqtest_audiodriver_wasapi_convert PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# One CTest entry per hardware case so a skipped case (no endpoint / exclusive refused) shows as
# "Skipped" on its own without hiding the others. Exit code 77 = skipped (sqtest SQ_SKIP).
sq_add_test(NAME audiodriver_wasapi_module SOURCES tests/test_module.cpp LIBS sq::appcore DEPENDS svc_audiodriver-wasapi
            ARGS "[enum]")
if(TARGET sqtest_audiodriver_wasapi_module)
  foreach(_case shared exclusive capture)
    add_test(NAME audiodriver_wasapi_${_case} COMMAND sqtest_audiodriver_wasapi_module "[${_case}]"
             WORKING_DIRECTORY "${SQ_BIN_DIR}")
    set_tests_properties(audiodriver_wasapi_${_case} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endforeach()
endif()
