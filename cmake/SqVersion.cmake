# Reads <root>/VERSION and exposes SQ_VERSION, SQ_VERSION_MAJOR/MINOR/PATCH, SQ_ABI_MAJOR, SQ_ABI_MINOR.
# Included BEFORE project() so project(VERSION) can use it.
function(sq_read_version_file root)
  file(STRINGS "${root}/VERSION" _lines REGEX "^SQ_[A-Z_]+=")
  foreach(_l IN LISTS _lines)
    string(REGEX MATCH "^([A-Z_]+)=(.*)$" _m "${_l}")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  endforeach()
endfunction()

sq_read_version_file("${CMAKE_CURRENT_LIST_DIR}/..")
if(NOT SQ_VERSION OR NOT DEFINED SQ_ABI_MAJOR OR NOT DEFINED SQ_ABI_MINOR)
  message(FATAL_ERROR "VERSION file is missing SQ_VERSION / SQ_ABI_MAJOR / SQ_ABI_MINOR")
endif()
string(REPLACE "." ";" _vparts "${SQ_VERSION}")
list(GET _vparts 0 SQ_VERSION_MAJOR)
list(GET _vparts 1 SQ_VERSION_MINOR)
list(GET _vparts 2 SQ_VERSION_PATCH)
