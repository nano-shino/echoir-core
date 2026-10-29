# End-to-end cases. The echoir tool transcribes a fixture, and `echoir score` scores the transcript
# (run_e2e.py). cases.json is read here, and each case becomes one test named e2e.<id>. A case whose
# model or fixture is missing exits 77, which ctest reports as skipped.
set(ECHOIR_E2E_CASES "${CMAKE_CURRENT_SOURCE_DIR}/cases.json")
# Editing the cases file reruns CMake, so the list of tests stays in step with it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ECHOIR_E2E_CASES}")

file(READ "${ECHOIR_E2E_CASES}" ECHOIR_E2E_JSON)
string(JSON ECHOIR_E2E_COUNT LENGTH "${ECHOIR_E2E_JSON}")
if(ECHOIR_E2E_COUNT GREATER 0)
  math(EXPR ECHOIR_E2E_LAST "${ECHOIR_E2E_COUNT} - 1")
  foreach(index RANGE ${ECHOIR_E2E_LAST})
    string(JSON case_id GET "${ECHOIR_E2E_JSON}" ${index} id)
    # A case with "long": true runs a long clip. Its streaming decode takes too long on CPU, so the
    # presets leave it out. Run it on a GPU build: ctest --test-dir build/windows-vulkan-x64 -L long.
    string(JSON case_long ERROR_VARIABLE case_long_error GET "${ECHOIR_E2E_JSON}" ${index} long)
    set(case_labels e2e)
    if(case_long_error STREQUAL "NOTFOUND" AND case_long)
      set(case_labels "e2e;long")
    endif()
    add_test(NAME e2e.${case_id}
      COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.py"
              --echoir $<TARGET_FILE:echoir_cli> --case ${case_id}
              --cases "${ECHOIR_E2E_CASES}" --source-dir "${PROJECT_SOURCE_DIR}" --show-text)
    set_tests_properties(e2e.${case_id} PROPERTIES LABELS "${case_labels}" SKIP_RETURN_CODE 77)
  endforeach()
endif()
