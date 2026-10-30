cmake_minimum_required(VERSION 3.24)

# echoir-core: portable C++20 real-time transcription core with Whisper and Qwen3-ASR backends.
project(echoir
  VERSION 3.0.0
  DESCRIPTION "Live audio to transcript core (Whisper and Qwen3-ASR)"
  LANGUAGES C CXX)

# --- Backend options ---------------------------------------------------------
# The engine's VADs are built with libwhisper (Silero runs through whisper_vad_*), so Qwen needs
# Whisper.
option(ECHOIR_WITH_WHISPER "Build the whisper.cpp backend and the Silero VAD" ON)
option(ECHOIR_WITH_QWEN "Build the Qwen3-ASR backend (llama.cpp libmtmd)" ON)
# A scripted backend and VAD replace the models, so sessions run without model files. Everything
# else (capture, catalogue, event queue) is the real code. Used by the binding and app tests.
option(ECHOIR_FAKE_BACKEND "Scripted backend and VAD in place of the models (no model files)" OFF)

if(ECHOIR_WITH_QWEN AND NOT ECHOIR_WITH_WHISPER)
  message(FATAL_ERROR
    "ECHOIR_WITH_QWEN=ON requires ECHOIR_WITH_WHISPER=ON, because the Silero VAD comes from libwhisper.")
endif()

# GPU options are passed through to ggml. The names match ggml's own options, so llama.cpp
# (cmake/backends.cmake) reads these same cache entries.
if(APPLE)
  set(ECHOIR_METAL_DEFAULT ON)
else()
  set(ECHOIR_METAL_DEFAULT OFF)
endif()
option(GGML_METAL "ggml Metal backend (Apple GPU)" ${ECHOIR_METAL_DEFAULT})
option(GGML_VULKAN "ggml Vulkan backend (Windows, Android)" OFF)
option(GGML_CUDA "ggml CUDA backend (optional)" OFF)

# --- Language and build settings ------------------------------------------------
# Set here, so they also apply to the backend submodules added in cmake/backends.cmake
# unless those set their own standard (ggml sets C++17 in its own tree).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables and shared libraries go to build/<preset>/bin, as llama.cpp and whisper.cpp do.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# The build RPATH is relative, so a copy of bin/ runs from any folder. tools/package-native.sh
# ships bin/ as it is. ELF takes $ORIGIN from CMAKE_BUILD_RPATH_USE_ORIGIN. That does nothing for
# Mach-O, so on Apple the RPATH is @loader_path. It is set here, before the backend subprojects
# are added, so ggml, whisper.cpp and llama.cpp inherit it too.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
if(APPLE)
  set(CMAKE_BUILD_RPATH "@loader_path")
endif()

# --- Shared targets ---------------------------------------------------------------
add_library(echoir_options INTERFACE)
if(MSVC)
  # /Zc:__cplusplus: report the real standard, as clang and gcc do.
  target_compile_options(echoir_options INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(echoir_options INTERFACE -Wall -Wextra)
endif()

# Vendored single headers: doctest, miniaudio, CLI11 and nlohmann/json.
# SYSTEM keeps their warnings out of our build.
add_library(echoir_single INTERFACE)
target_include_directories(echoir_single SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/single")

# --- Backends ----------------------------------------------------------------------
# cmake/backends.cmake adds ggml (once, from llama.cpp), libllama, libmtmd and libwhisper,
# and sets ECHOIR_BUILD_WHISPER and ECHOIR_BUILD_QWEN to what was really built.
include(cmake/backends.cmake)

# Android: ggml's OpenMP is the NDK's libomp.so, which Android does not ship. Copy it into the
# output folder, so the native build is complete for the binding and the CLI.
if(ANDROID AND GGML_OPENMP_ENABLED)
  if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "libomp.so is located only for arm64-v8a; add its NDK path for ${CMAKE_ANDROID_ARCH_ABI}")
  endif()
  file(GLOB ECHOIR_ANDROID_LIBOMP
    "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/aarch64/libomp.so")
  list(LENGTH ECHOIR_ANDROID_LIBOMP ECHOIR_ANDROID_LIBOMP_COUNT)
  if(NOT ECHOIR_ANDROID_LIBOMP_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one NDK libomp.so for aarch64, found ${ECHOIR_ANDROID_LIBOMP_COUNT}")
  endif()
  add_custom_target(echoir_android_libomp ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${ECHOIR_ANDROID_LIBOMP}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libomp.so")
endif()

# Set after the submodules, so their own targets are built without it.
if(ECHOIR_FAKE_BACKEND)
  add_compile_definitions(ECHOIR_FAKE_BACKEND=1)
endif()

# --- Core ------------------------------------------------------------------------
add_subdirectory(src)

# --- Command-line tool ------------------------------------------------------------
# The tool links the engine (VAD and backends), so it exists only when that does.
if(TARGET echoir_engine)
  add_subdirectory(cli)
endif()

# --- Tests ------------------------------------------------------------------------
include(CTest)  # defines BUILD_TESTING (default ON) and enables ctest
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
