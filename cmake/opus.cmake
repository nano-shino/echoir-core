# libopus, the codec of the session tape's audio pages. Included from the top-level CMakeLists.txt.
#
# Built from the third_party/opus submodule as a static library with position-independent code,
# so it links into the shared echoir library (and into the static archives of the mobile builds)
# without shipping a separate opus library. Only the library target is used: its tests, programs
# and install rules are off, and EXCLUDE_FROM_ALL builds it only because echoir links it.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/CMakeLists.txt")
  message(FATAL_ERROR
    "third_party/opus is not checked out. Run 'git submodule update --init'.")
endif()

# The settings are normal variables of this function's scope, so they reach the opus directory
# and nothing else: the cache and the rest of the build keep their own values.
function(echoir_add_opus)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(OPUS_BUILD_SHARED_LIBRARY OFF)
  set(OPUS_BUILD_TESTING OFF)
  set(OPUS_BUILD_PROGRAMS OFF)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
  add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/opus"
    "${PROJECT_BINARY_DIR}/third_party/opus" EXCLUDE_FROM_ALL)
endfunction()
echoir_add_opus()
