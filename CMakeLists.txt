cmake_minimum_required(VERSION 3.25)
project(mpcboy LANGUAGES C CXX)

set(MPCBOY_TARGET "native" CACHE STRING "native | arm32 | arm64 | mac-vst")
option(MPCBOY_CPU_PROBE "Build the WP3 CPU spike plugin (debug only; shows mean/p99 per block in dbg_us)" OFF)
set(SDK "${CMAKE_SOURCE_DIR}/vendor/mpc-plugin-builder-sdk")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)            # gnu11, as rackboy uses for SameBoy apu.c
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# -O2 without NDEBUG (matches the SDK flags and rackboy's test flags).
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)

# SameBoy's apu.h is included by the vendored shim as "vendor/sameboy/apu.h":
# third_party/vendor/sameboy/ with third_party on the include path resolves it
# with no generated files and no edit to the vendored shim.

# ---- gbcore: host-agnostic Game Boy core (no SDK headers) -------------------
file(GLOB GBCORE_SRC CONFIGURE_DEPENDS
  src/apu/*.c src/core/*.cpp src/table/*.cpp src/sav/*.cpp src/dsp/*.cpp
  src/engine/*.cpp)
# The vendored reference resampler is test-only (the engine uses the polyphase ones:
# PolyphaseResampler48To441 and RateResampler; the tests compare against it).
list(REMOVE_ITEM GBCORE_SRC "${CMAKE_SOURCE_DIR}/src/dsp/rackboy_resampler.cpp")
list(APPEND GBCORE_SRC third_party/vendor/sameboy/apu.c)
add_library(gbcore STATIC ${GBCORE_SRC})
target_include_directories(gbcore PUBLIC
  src/apu src/core src/table src/sav src/dsp src/engine third_party)

# ---- plugin ----------------------------------------------------------------
# plugincore holds everything except the VSTPluginMain entry point so the native
# tests can drive MpcBoyPlugin directly.
file(GLOB PLUGINCORE_SRC CONFIGURE_DEPENDS src/plugin/*.cpp)
list(REMOVE_ITEM PLUGINCORE_SRC "${CMAKE_SOURCE_DIR}/src/plugin/plugin_entry.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/plugin_entry_mac.cpp")
add_library(plugincore STATIC ${PLUGINCORE_SRC})
target_include_directories(plugincore PUBLIC "${SDK}/framework" src/plugin)
target_link_libraries(plugincore PUBLIC gbcore)
# Owned code only (the vendored SameBoy/rackboy sources in gbcore keep their warnings).
target_compile_options(plugincore PRIVATE -Werror)
# The .sav import worker (WP11) and, in probe builds, the stats worker use std::thread.
find_package(Threads REQUIRED)
target_link_libraries(plugincore PUBLIC Threads::Threads)
if(MPCBOY_CPU_PROBE)
  target_compile_definitions(plugincore PUBLIC MPCBOY_CPU_PROBE=1)
endif()

# ---- shared UI interaction model (UI WP2) ------------------------------------
# Platform-free; used by the native tests and the Mac editor only (the MPC firmware has its own
# interaction, so the arm builds never compile it).
if(MPCBOY_TARGET STREQUAL "native" OR MPCBOY_TARGET STREQUAL "mac-vst")
  add_library(mpcboy_ui STATIC src/ui/ui_model.cpp)
  target_include_directories(mpcboy_ui PUBLIC src/ui)
  target_link_libraries(mpcboy_ui PUBLIC gbcore)
  target_compile_options(mpcboy_ui PRIVATE -Werror)
endif()

if(MPCBOY_TARGET STREQUAL "mac-vst")
  include(cmake/mac_vst.cmake)   # mpcboy.vst for Ableton Live on a Mac (dev build)
else()
  add_library(mpcboy_plugin MODULE src/plugin/plugin_entry.cpp)
  target_link_libraries(mpcboy_plugin PRIVATE plugincore)
  set_target_properties(mpcboy_plugin PROPERTIES PREFIX "")
  # A probe build must never be mistaken for the release artefact: it gets its own file name.
  if(MPCBOY_CPU_PROBE)
    set(_probe_suffix "-probe")
  else()
    set(_probe_suffix "")
  endif()
  if(MPCBOY_TARGET STREQUAL "native")
    set_target_properties(mpcboy_plugin PROPERTIES OUTPUT_NAME mpcboy${_probe_suffix} SUFFIX ".dylib")
  else()
    set_target_properties(mpcboy_plugin PROPERTIES OUTPUT_NAME mpcboy${_probe_suffix}-${MPCBOY_TARGET} SUFFIX ".so")
    target_link_options(mpcboy_plugin PRIVATE
      -Wl,--gc-sections -Wl,-s "-Wl,--version-script=${SDK}/tools/exports.map")
  endif()
endif()

# ---- native-only: tests, bench, host simulator -----------------------------
if(MPCBOY_TARGET STREQUAL "native")
  enable_testing()

  file(GLOB_RECURSE RACKBOY_TEST_SRC CONFIGURE_DEPENDS
    tests/rackboy/apu/*.cpp tests/rackboy/core/*.cpp tests/rackboy/table/*.cpp
    tests/rackboy/sav/*.cpp tests/rackboy/resampler/*.cpp tests/rackboy/audio/*.cpp
    tests/rackboy/common/*.cpp tests/rackboy/support/*.cpp)
  file(GLOB MPCBOY_TEST_SRC CONFIGURE_DEPENDS tests/unit/*.cpp)
  add_executable(mpcboy_tests tests/rackboy/doctest_main.cpp
    ${RACKBOY_TEST_SRC} ${MPCBOY_TEST_SRC} src/dsp/rackboy_resampler.cpp)
  target_include_directories(mpcboy_tests PRIVATE
    tests/third_party tests/rackboy tests/rackboy/support)
  target_link_libraries(mpcboy_tests PRIVATE plugincore mpcboy_ui)   # exports gbcore
  # Copied rackboy suites find fixtures/ and audio/ relative to the cwd.
  add_test(NAME rackboy_suites COMMAND mpcboy_tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/rackboy")
  # A skipped golden regression must never pass silently.
  set_tests_properties(rackboy_suites PROPERTIES FAIL_REGULAR_EXPRESSION "skipping RMS check")

  add_executable(mpcboy_bench bench/bench_main.cpp)
  target_link_libraries(mpcboy_bench PRIVATE gbcore)

  add_executable(host_sim "${SDK}/tools/host-sim/host_sim.cpp")
  target_include_directories(host_sim PRIVATE "${SDK}/framework")
  if(NOT APPLE)
    target_link_libraries(host_sim PRIVATE ${CMAKE_DL_LIBS})
  endif()
  set_target_properties(host_sim PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  # WP7 test 10: the SDK host simulator against the native plugin (exit 0 = no FAIL).
  add_test(NAME host_sim COMMAND host_sim "$<TARGET_FILE:mpcboy_plugin>" test
           --seconds 4 --notes 48,60 --playing)
  set_tests_properties(host_sim PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL  |WARN  [^\n]*clip")
endif()
