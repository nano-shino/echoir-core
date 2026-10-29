# Backend wiring: one ggml, shared by whisper.cpp and llama.cpp. Included from the top-level
# CMakeLists.txt.
#
# ggml is built ONCE, from third_party/llama.cpp/ggml, by llama.cpp's own build. Both upstream
# trees skip their own ggml when a `ggml` target already exists (`if (NOT TARGET ggml)`), so
# whisper.cpp links the same target. The order matters: llama.cpp is added before whisper.cpp,
# because whisper.cpp added first would build its own ggml from its tree. WHISPER_USE_SYSTEM_GGML
# is not needed, since the target already exists.
#
# Only libraries are built from the submodules: libllama, libmtmd (llama.cpp tools/mtmd) and
# libwhisper. Their examples, tools, tests, server and common utilities are all off.

# --- Shared or static, for the whole tree ---------------------------------------
# ON is the desktop default. BUILD_SHARED_LIBS=OFF (preset macos-static) links ggml, libwhisper,
# libllama and libmtmd statically. ggml, llama.cpp and whisper.cpp all read this same variable.
option(BUILD_SHARED_LIBS "Build ggml, libwhisper, libllama and libmtmd as shared libraries" ON)

# --- Which backends are really built ----------------------------------------------
# A backend whose submodule is missing is skipped with a warning. A clone without
# submodules still configures and runs the unit tests. ggml comes from llama.cpp, so libwhisper
# needs that submodule too. Qwen also needs the Silero VAD from libwhisper.
set(ECHOIR_HAS_LLAMA_SRC OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
  set(ECHOIR_HAS_LLAMA_SRC ON)
endif()
set(ECHOIR_HAS_WHISPER_SRC OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp/CMakeLists.txt")
  set(ECHOIR_HAS_WHISPER_SRC ON)
endif()

set(ECHOIR_BUILD_WHISPER ${ECHOIR_WITH_WHISPER})
set(ECHOIR_BUILD_QWEN ${ECHOIR_WITH_QWEN})

if(ECHOIR_BUILD_WHISPER AND NOT (ECHOIR_HAS_LLAMA_SRC AND ECHOIR_HAS_WHISPER_SRC))
  message(WARNING
    "ECHOIR_WITH_WHISPER=ON, but third_party/whisper.cpp or third_party/llama.cpp is not checked "
    "out. Run 'git submodule update --init'. The whisper backend and VAD are skipped.")
  set(ECHOIR_BUILD_WHISPER OFF)
endif()
if(ECHOIR_BUILD_QWEN AND NOT (ECHOIR_HAS_LLAMA_SRC AND ECHOIR_BUILD_WHISPER))
  message(WARNING
    "ECHOIR_WITH_QWEN=ON, but the llama.cpp or whisper.cpp submodule is missing, or the whisper "
    "backend is off. The Qwen backend is skipped.")
  set(ECHOIR_BUILD_QWEN OFF)
endif()

# --- llama.cpp: ggml, libllama and (for Qwen) libmtmd ---------------------------
# llama.cpp is added whenever any backend is on, so ggml gets the same configuration in every
# backend combination. llama.cpp's own defaults for ggml (for example GGML_LLAMAFILE) apply.
# Normal variables, not cache entries: llama.cpp's option() calls respect them (CMP0077 NEW).
# The submodules are C++17 projects. Our C++20 default would leak into them, and under C++20 on
# MSVC (where __cplusplus stays 199711L) llama-chat.cpp's u8 literals become char8_t and fail.
# The standard is restored after both are added (included at top level, so this scope is ours).
set(ECHOIR_CXX_STANDARD_SAVED ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)

if(ECHOIR_BUILD_WHISPER OR ECHOIR_BUILD_QWEN)
  set(LLAMA_BUILD_COMMON    OFF)
  set(LLAMA_BUILD_TESTS     OFF)
  set(LLAMA_BUILD_TOOLS     OFF)  # also turns off tools/mtmd's CLI executables
  set(LLAMA_BUILD_EXAMPLES  OFF)
  set(LLAMA_BUILD_SERVER    OFF)
  set(LLAMA_BUILD_APP       OFF)
  # llama.cpp adds tools/mtmd only through this option, and only while LLAMA_BUILD_TOOLS is OFF.
  set(LLAMA_BUILD_MTMD      ${ECHOIR_BUILD_QWEN})

  # Adds ggml (as no `ggml` target exists yet), then libllama and libmtmd.
  add_subdirectory(third_party/llama.cpp)
endif()

if(ECHOIR_BUILD_QWEN AND NOT TARGET mtmd)
  message(FATAL_ERROR "ECHOIR_WITH_QWEN=ON, but llama.cpp did not add the mtmd target.")
endif()

# --- whisper.cpp: libwhisper (also the Silero VAD) ------------------------------
# Built after llama.cpp, so the `ggml` target above already exists and whisper.cpp skips its own.
# Its tests, examples and server default to OFF outside a standalone build, so they are not set
# here. Setting them as normal variables would make whisper's option() calls warn (its
# cmake_minimum_required is 3.5, so CMP0077 is unset there).
if(ECHOIR_BUILD_WHISPER)
  if(NOT TARGET ggml)
    message(FATAL_ERROR "whisper.cpp would build its own ggml. Keep llama.cpp before whisper.cpp.")
  endif()
  add_subdirectory(third_party/whisper.cpp)
  # whisper.cpp also builds parakeet, a model family this core does not use. Nothing here links it,
  # so it stays out of the default build and bin/ holds no parakeet library.
  if(TARGET parakeet)
    set_target_properties(parakeet PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endif()

set(CMAKE_CXX_STANDARD ${ECHOIR_CXX_STANDARD_SAVED})
