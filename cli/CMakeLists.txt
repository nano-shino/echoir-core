# The echoir command-line tool. It links the engine directly rather than the C ABI, because the
# offline path has no C ABI entry point. The target is named echoir_cli, since echoir is the
# library target. The executable is still called echoir and lands in build/<preset>/bin.
#
# The command code lives in echoir_cli_lib, so a test can link it. main.cpp only parses the
# command line.
add_library(echoir_cli_lib STATIC bench.cpp base64.cpp devices_cmd.cpp input.cpp
  models.cpp models_cmd.cpp offline.cpp output.cpp model_dirs.cpp score.cpp serve.cpp stream.cpp)
target_include_directories(echoir_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(echoir_cli_lib PUBLIC
  echoir_engine echoir_pure echoir_single)
target_link_libraries(echoir_cli_lib PRIVATE echoir_options)

add_executable(echoir_cli main.cpp)
set_target_properties(echoir_cli PROPERTIES OUTPUT_NAME echoir PDB_NAME echoir_cli)
target_link_libraries(echoir_cli PRIVATE echoir_cli_lib echoir_options)
