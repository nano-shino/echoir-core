# Runs the command-line tool with ARGS and checks its result. The exit status must be EXPECT, and
# standard error must match MESSAGE. When OUT_MATCHES is set, standard output must match it too.
# CTest's regular-expression properties ignore the exit status, so usage tests use this script.
# ARGS separates its arguments with '|', because a semicolon does not survive add_test. When STDIN
# is set, it is a file that standard input is read from.
string(REPLACE "|" ";" ARG_LIST "${ARGS}")
if(DEFINED STDIN)
  execute_process(COMMAND "${ECHOIR}" ${ARG_LIST}
    INPUT_FILE "${STDIN}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 300)
else()
  execute_process(COMMAND "${ECHOIR}" ${ARG_LIST}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 300)
endif()

if(NOT status STREQUAL "${EXPECT}")
  message(FATAL_ERROR "exit status ${status}, expected ${EXPECT}\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT err MATCHES "${MESSAGE}")
  message(FATAL_ERROR "stderr does not match '${MESSAGE}':\n${err}")
endif()
if(DEFINED OUT_MATCHES AND NOT out MATCHES "${OUT_MATCHES}")
  message(FATAL_ERROR "stdout does not match '${OUT_MATCHES}':\n${out}")
endif()
