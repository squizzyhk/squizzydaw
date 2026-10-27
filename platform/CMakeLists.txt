# sqplatform: foundation DLL with a stable C ABI (platform/include/sq/platform*.h). Target: sq::platform
add_library(sqplatform SHARED
  src/core.cpp
  src/thread.cpp
  src/system.cpp
  src/fs.cpp
  src/text.cpp
  src/xml.cpp
  src/json.cpp
  src/hash.cpp
  src/log.cpp
  src/ipc_win.cpp
  src/ipc_posix.cpp
)
add_library(sq::platform ALIAS sqplatform)
sq_target_defaults(sqplatform)
target_include_directories(sqplatform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sqplatform PUBLIC sq::sdk)
target_compile_definitions(sqplatform PRIVATE SQ_PLATFORM_BUILD=1)
sq_set_output_dir(sqplatform "${SQ_BIN_DIR}")
sq_stamp_version(sqplatform sqplatform ${SQ_VERSION} "Squizzy Studio platform layer" "VFT_DLL")

if(WIN32)
  target_link_libraries(sqplatform PRIVATE shell32 ole32 bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(sqplatform PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(sqplatform PRIVATE rt)
  endif()
endif()
