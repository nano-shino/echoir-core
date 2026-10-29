# Unit tests run with no models. Every tests/unit/*.cpp joins one doctest runner.
# CONFIGURE_DEPENDS makes a newly added file show up on the next build.
file(GLOB ECHOIR_UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")

find_package(Threads REQUIRED)

add_executable(echoir_unit_tests ${ECHOIR_UNIT_SOURCES})
# The version test compares echoir_version() with the project version. Tests that read fixtures
# use the source tree.
target_compile_definitions(echoir_unit_tests PRIVATE
  ECHOIR_EXPECTED_VERSION="${PROJECT_VERSION}" ECHOIR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(echoir_unit_tests PRIVATE
  echoir_options echoir_single echoir_pure echoir Threads::Threads)

add_test(NAME unit COMMAND echoir_unit_tests)
set_tests_properties(unit PROPERTIES LABELS unit)

# C99 check of the public header. The source is plain C, so this proves echoir.h compiles as
# C99 and links against echoir. It does not use doctest.
add_executable(echoir_c_header_test unit/test_header_c.c)
set_target_properties(echoir_c_header_test PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
target_link_libraries(echoir_c_header_test PRIVATE echoir_options echoir)

add_test(NAME c_header COMMAND echoir_c_header_test)
set_tests_properties(c_header PROPERTIES LABELS unit)

# Live-mode stress test: three pushers at the real-time rate, while start, stop, clear and reset run
# at random for 60 s. It runs on the scripted backend and VAD, so it needs no model. It is labelled
# long, so the default test presets leave it out. The macos-tsan preset runs it under
# ThreadSanitizer.
add_executable(echoir_stress_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/stress/test_live_stress.cpp")
target_link_libraries(echoir_stress_tests PRIVATE
  echoir_options echoir_single echoir_pure Threads::Threads)

add_test(NAME stress_live COMMAND echoir_stress_tests)
set_tests_properties(stress_live PROPERTIES LABELS "long;stress" TIMEOUT 900)

# Integration tests need real models. Every tests/integration/*.cpp joins one doctest runner,
# except the C ABI tests, which have their own runner below. A test whose model or fixture is
# missing prints SKIP and passes (test_support.h).
if(TARGET echoir_engine)
  file(GLOB ECHOIR_INTEGRATION_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp")
  list(REMOVE_ITEM ECHOIR_INTEGRATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_capi_engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_capi_capture.cpp")
  # The fake build has no real models, so these cases (which check real decoder output) are left out.
  if(NOT ECHOIR_FAKE_BACKEND)
  add_executable(echoir_integration_tests ${ECHOIR_INTEGRATION_SOURCES})
  target_compile_definitions(echoir_integration_tests PRIVATE
    ECHOIR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(echoir_integration_tests PRIVATE
    echoir_options echoir_single echoir_engine Threads::Threads)

  # The long-session cases run about ten minutes of audio each. The integration test leaves them
  # out, and the test presets exclude the long label, so they run only by name:
  # ctest -R long_session. They run alone (RUN_SERIAL), so other tests do not slow their timing.
  # The Qwen streaming golden cases take about 100 s together, and their Whisper twins cover the
  # same session logic, so they also carry the long label: ctest -R qwen_stream_golden.
  add_test(NAME integration COMMAND echoir_integration_tests
    "-tce=long session*,stream session * with qwen")
  set_tests_properties(integration PROPERTIES LABELS integration)
  add_test(NAME long_session COMMAND echoir_integration_tests "-tc=long session*")
  # The pass line needs at least four cases run and none failed. Without it, a filter that matches
  # nothing would pass, since doctest exits 0 when no case fails.
  set_tests_properties(long_session PROPERTIES LABELS long TIMEOUT 3600 RUN_SERIAL TRUE
    PASS_REGULAR_EXPRESSION [=[test cases: +([4-9]|[1-9][0-9]+) \| +([4-9]|[1-9][0-9]+) passed \| +0 failed]=])
  # On macOS a missing model or fixture fails this test instead of passing with every case skipped.
  # Other hosts do not set it: their runs lack the long_mix fixture and the Qwen models.
  add_test(NAME qwen_stream_golden COMMAND echoir_integration_tests
    "-tc=stream session * with qwen")
  set_tests_properties(qwen_stream_golden PROPERTIES LABELS long TIMEOUT 900
    PASS_REGULAR_EXPRESSION [=[test cases: +([2-9]|[1-9][0-9]+) \| +([2-9]|[1-9][0-9]+) passed \| +0 failed]=])
  if(APPLE)
    set_tests_properties(long_session qwen_stream_golden PROPERTIES
      ENVIRONMENT ECHOIR_REQUIRE_MODELS=1)
  endif()
  endif()  # NOT ECHOIR_FAKE_BACKEND

  # The C ABI tests link the echoir library, plus echoir_pure for the text helpers that
  # test_support.h uses. They do not link the engine objects: a second copy would give the process
  # two sets of backend logging state, and the second setup would replace the first.
  add_executable(echoir_capi_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_capi_engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_capi_capture.cpp")
  target_compile_definitions(echoir_capi_tests PRIVATE
    ECHOIR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(echoir_capi_tests PRIVATE echoir_options echoir_single echoir echoir_pure)

  # The capture cases need a device, so the run uses miniaudio's null backend. It is silent and
  # present everywhere, and the engine cases do not open audio.
  add_test(NAME capi COMMAND echoir_capi_tests)
  set_tests_properties(capi PROPERTIES LABELS integration ENVIRONMENT ECHOIR_AUDIO_BACKEND=null)
endif()

# The command-line tool answers --help, and scores text, without any model. Scoring a text against
# itself must succeed. The rates themselves are checked by the end-to-end cases.
if(TARGET echoir_cli)
  add_test(NAME cli_help COMMAND echoir_cli --help)
  set_tests_properties(cli_help PROPERTIES LABELS unit)

  add_test(NAME cli_score COMMAND echoir_cli score
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.ref.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.ref.txt")
  # PASS_REGULAR_EXPRESSION replaces the exit status check, so the output line must match exactly.
  set_tests_properties(cli_score PROPERTIES LABELS unit
    PASS_REGULAR_EXPRESSION [=["rate":0,"subs":0,"dels":0,"ins":0,"ref_len":22]=])

  # --vad-param is a development option, so it stays out of the help text. A malformed one is a
  # usage error, reported before any model is read.
  add_test(NAME cli_transcribe_help_hides_vad_param COMMAND echoir_cli transcribe --help)
  set_tests_properties(cli_transcribe_help_hides_vad_param PROPERTIES LABELS unit
    FAIL_REGULAR_EXPRESSION "vad-param")
  add_test(NAME cli_vad_param_syntax COMMAND echoir_cli transcribe
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.wav" --model whisper-tiny-en --offline
    --vad-param threshold)
  set_tests_properties(cli_vad_param_syntax PROPERTIES LABELS unit
    PASS_REGULAR_EXPRESSION "error: --vad-param expects key=value")
  # A well-formed setting reaches the VAD, which rejects a key it does not have. Skipped when the
  # models are not fetched.
  if(NOT ECHOIR_FAKE_BACKEND)
    add_test(NAME cli_vad_param_reaches_vad COMMAND echoir_cli transcribe
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.wav" --model whisper-tiny-en --offline
      --vad-param bogus=1)
    set_tests_properties(cli_vad_param_reaches_vad PROPERTIES LABELS unit
      PASS_REGULAR_EXPRESSION "unknown setting for the Silero VAD: bogus"
      SKIP_REGULAR_EXPRESSION "fetch-models")
  endif()

  # Usage tests check the exit status (2) and the message. Each of these is reported before any
  # model is read. jfk.wav stands in for a model file, since it exists and is not a model.
  set(JFK "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.wav")
  function(add_usage_test name args message)
    set(extra ${ARGN})
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
      "-DECHOIR=$<TARGET_FILE:echoir_cli>" "-DARGS=${args}" "-DEXPECT=2" "-DMESSAGE=${message}"
      ${extra} -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_usage.cmake")
    set_tests_properties(${name} PROPERTIES LABELS unit)
  endfunction()

  # A transcription names a mode. Streaming is the only one that paces input.
  add_usage_test(cli_transcribe_needs_a_mode
    "transcribe|${JFK}|--model|whisper-tiny-en"
    "pass --offline to transcribe the whole file")
  add_usage_test(cli_offline_excludes_stream
    "transcribe|${JFK}|--model|whisper-tiny-en|--offline|--stream"
    "--offline excludes --stream")
  # A model file names no backend, so the backend must be given. A stream reports the failure as
  # an error event on standard output, as jsonl's error object.
  add_usage_test(cli_model_file_needs_backend
    "transcribe|${JFK}|--model|${JFK}|--stream"
    "--backend is required when --model is a file")
  add_usage_test(cli_stream_usage_error_is_an_event
    "transcribe|${JFK}|--model|${JFK}|--stream|--format|jsonl"
    "--backend is required when --model is a file"
    "-DOUT_MATCHES=type.*error")
  # The context comes from one of two flags. A missing context file fails before the models load.
  add_usage_test(cli_context_and_context_file_exclude
    "transcribe|${JFK}|--model|whisper-tiny-en|--offline|--context|names|--context-file|${JFK}"
    "--context-file")
  add_test(NAME cli_context_file_missing COMMAND ${CMAKE_COMMAND}
    "-DECHOIR=$<TARGET_FILE:echoir_cli>"
    "-DARGS=transcribe|${JFK}|--model|whisper-tiny-en|--offline|--context-file|no-such-context.txt"
    "-DEXPECT=1" "-DMESSAGE=cannot read context file"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_usage.cmake")
  set_tests_properties(cli_context_file_missing PROPERTIES LABELS unit)

  # The stabilization policy is a streaming option. Rollback needs a backend with decoder prefix
  # support, and Whisper has none. That refusal comes after the model loads, so that test needs
  # whisper-tiny-en, and it skips when the model is not fetched.
  add_usage_test(cli_policy_needs_stream
    "transcribe|${JFK}|--model|whisper-tiny-en|--offline|--policy|rollback"
    "--policy requires --stream")
  if(NOT ECHOIR_FAKE_BACKEND)
    add_test(NAME cli_policy_rollback_whisper COMMAND echoir_cli transcribe
      "${JFK}" --model whisper-tiny-en --stream --policy rollback)
    set_tests_properties(cli_policy_rollback_whisper PROPERTIES LABELS unit
      PASS_REGULAR_EXPRESSION "the rollback policy is not supported by the whisper backend"
      SKIP_REGULAR_EXPRESSION "fetch-models")
  endif()
  # The minimum chunk is a streaming option as well, and it is a usage error without --stream.
  add_usage_test(cli_min_chunk_ms_needs_stream
    "transcribe|${JFK}|--model|whisper-tiny-en|--offline|--min-chunk-ms|1000"
    "--min-chunk-ms requires --stream")
  add_usage_test(cli_max_segment_ms_needs_stream
    "transcribe|${JFK}|--model|whisper-tiny-en|--offline|--max-segment-ms|10000"
    "--max-segment-ms requires --stream")
  # The input is a file or -, given once: as the positional argument or with --input. Raw samples
  # need standard input and a format. These are all reported before any model is read.
  add_usage_test(cli_input_is_required
    "transcribe|--model|whisper-tiny-en|--offline"
    "give the input as a WAV file")
  add_usage_test(cli_input_and_file_exclude
    "transcribe|${JFK}|--input|${JFK}|--model|whisper-tiny-en|--offline"
    "excludes")
  add_usage_test(cli_pcm_needs_standard_input
    "transcribe|${JFK}|--pcm|s16le|--rate|16000|--channels|1|--model|whisper-tiny-en|--offline"
    "--pcm needs standard input")
  add_usage_test(cli_pcm_needs_rate
    "transcribe|-|--pcm|s16le|--channels|2|--model|whisper-tiny-en|--offline"
    "--pcm requires --rate")
  add_usage_test(cli_rate_needs_pcm
    "transcribe|-|--rate|16000|--model|whisper-tiny-en|--offline"
    "--rate requires --pcm")
  # Standard input that is neither a WAV nor raw samples with a format. The text file has no RIFF
  # header, and the error comes before any model is read.
  add_usage_test(cli_stdin_not_wav
    "transcribe|-|--model|whisper-tiny-en|--offline"
    "--pcm s16le"
    "-DSTDIN=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jfk.ref.txt")
  # Live capture is a streaming input from a device. Its usage errors are reported before any
  # device or model is opened, so these need neither.
  add_usage_test(cli_capture_needs_stream
    "transcribe|--input|mic|--model|whisper-tiny-en|--offline"
    "live capture needs --stream")
  add_usage_test(cli_capture_rejects_pcm
    "transcribe|--input|mic|--pcm|s16le|--rate|16000|--channels|1|--model|whisper-tiny-en|--stream"
    "--pcm needs standard input")
  add_usage_test(cli_duration_needs_capture
    "transcribe|${JFK}|--model|whisper-tiny-en|--stream|--duration|5"
    "--duration needs --input mic or --input loopback")
  # Loopback is WASAPI, so elsewhere the error names the loopback device to use instead.
  if(NOT WIN32)
    add_usage_test(cli_loopback_needs_windows
      "transcribe|--input|loopback|--model|whisper-tiny-en|--stream"
      "BlackHole")
  endif()

  # An id that is not in the registry is a usage error. The registry is read from the source tree,
  # so the test needs no model files.
  add_usage_test(cli_models_path_unknown_id "models|path|no-such-id"
    "no-such-id is not a registry id")

  # The end-to-end cases run the real tool on fixtures. Their runner needs Python, so they exist
  # only when Python is found.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND AND NOT ECHOIR_FAKE_BACKEND)
    add_subdirectory(e2e)
  endif()
endif()

# The stdio service's protocol tests. They link the command code and drive the service through
# string streams with scripted models. They are not in tests/unit: that directory is one binary
# that must not link the engine twice.
if(TARGET echoir_cli_lib)
  add_executable(echoir_serve_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/cli/test_serve.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_main.cpp")
  target_link_libraries(echoir_serve_tests PRIVATE
    echoir_cli_lib echoir_options echoir_single Threads::Threads)

  add_test(NAME serve COMMAND echoir_serve_tests)
  set_tests_properties(serve PROPERTIES LABELS unit)
endif()

# Sessions on the scripted backend and VAD. Only the ECHOIR_FAKE_BACKEND build has them.
if(ECHOIR_FAKE_BACKEND)
  add_executable(echoir_fake_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fake/test_fake_engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_main.cpp")
  target_link_libraries(echoir_fake_tests PRIVATE
    echoir_options echoir_single echoir Threads::Threads)

  add_test(NAME fake_engine COMMAND echoir_fake_tests)
  set_tests_properties(fake_engine PROPERTIES LABELS fake)
endif()
