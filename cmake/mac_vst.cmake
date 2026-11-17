# macOS VST2 bundle (mpcboy.vst) for desktop hosts such as Ableton Live. Development build:
# it is not part of the MPC package. Mirrors the SDK's tools/build.py build_desktop():
#   mpcboy.vst/Contents/{Info.plist, PkgInfo, MacOS/mpcboy, Resources/Content/Import/README.txt}
# universal (arm64 + x86_64, set by the mac-vst preset), only VSTPluginMain and main_macho
# exported, ad-hoc codesigned after every build.
if(NOT APPLE)
  message(FATAL_ERROR "MPCBOY_TARGET=mac-vst only builds on macOS")
endif()

# Identity from plugin/plugin.json (single source of truth, shared with the MPC package).
file(READ "${CMAKE_SOURCE_DIR}/plugin/plugin.json" _pj)
string(JSON MPCBOY_VERSION GET "${_pj}" version)
string(JSON MPCBOY_MANUFACTURER GET "${_pj}" manufacturer)
string(JSON MPCBOY_UID GET "${_pj}" uid)
string(TOLOWER "${MPCBOY_MANUFACTURER}" _mfr)
set(MPCBOY_BUNDLE_ID "com.${_mfr}.mpcboy")
configure_file("${CMAKE_SOURCE_DIR}/cmake/Info.plist.in" "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/plugin/plugin.json")

add_library(mpcboy_macho MODULE src/plugin/plugin_entry.cpp)
target_link_libraries(mpcboy_macho PRIVATE plugincore)
set_target_properties(mpcboy_macho PROPERTIES
  PREFIX "" SUFFIX "" OUTPUT_NAME mpcboy
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/macho")
target_link_options(mpcboy_macho PRIVATE
  -Wl,-dead_strip
  "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/cmake/mac_exports.txt")
set_property(TARGET mpcboy_macho APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/mac_exports.txt")

add_custom_command(TARGET mpcboy_macho POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
    "-DBINARY=$<TARGET_FILE:mpcboy_macho>"
    "-DBUNDLE=${CMAKE_BINARY_DIR}/mpcboy.vst"
    "-DPLIST=${CMAKE_BINARY_DIR}/Info.plist"
    "-DUID=${MPCBOY_UID}"
    -P "${CMAKE_SOURCE_DIR}/cmake/assemble_mac_bundle.cmake"
  COMMENT "Assembling and ad-hoc signing mpcboy.vst"
  VERBATIM)
