# The C ABI library. Added by src/CMakeLists.txt. The public header is include/echoir/echoir.h.
# The library is SHARED or STATIC, following BUILD_SHARED_LIBS.
file(GLOB ECHOIR_CAPI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(echoir ${ECHOIR_CAPI_SOURCES})

target_include_directories(echoir PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(echoir PRIVATE
  ECHOIR_BUILD                           # dllexport on Windows while building the library
  ECHOIR_VERSION_STRING="${PROJECT_VERSION}")
if(NOT BUILD_SHARED_LIBS)
  # Consumers of a static build must not expect dllimport (see echoir.h).
  target_compile_definitions(echoir PUBLIC ECHOIR_STATIC)
endif()

target_link_libraries(echoir PRIVATE echoir_options)

# With the engine (VAD and backends) built, the C ABI creates engines. The engine links echoir_pure
# itself, so it is not listed twice. Without the engine, engine create reports that the backends
# are not built.
if(TARGET echoir_engine)
  target_link_libraries(echoir PRIVATE echoir_engine)
  target_compile_definitions(echoir PRIVATE ECHOIR_HAS_ENGINE=1)
else()
  target_link_libraries(echoir PRIVATE echoir_pure)
endif()

# echoir_pure is a static library that gets linked into the shared echoir, so it needs PIC.
# Linux needs this explicitly. macOS is PIC by default.
set_target_properties(echoir_pure PROPERTIES POSITION_INDEPENDENT_CODE ON)
