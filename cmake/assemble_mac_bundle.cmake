# cmake -P script run by the mpcboy_vst custom command: builds mpcboy.vst from scratch and ad-hoc signs it.
# Inputs: BINARY, BUNDLE, PLIST, UID, IMPORT_README (plugin/Content/Import/README.txt, shared with
# scripts/package_mpc.py for the MPC folder).
file(REMOVE_RECURSE "${BUNDLE}")
file(MAKE_DIRECTORY "${BUNDLE}/Contents/MacOS" "${BUNDLE}/Contents/Resources/Content/Import")
file(COPY "${BINARY}" DESTINATION "${BUNDLE}/Contents/MacOS")
file(COPY "${PLIST}" DESTINATION "${BUNDLE}/Contents")
string(SUBSTRING "${UID}????" 0 4 _sig)
file(WRITE "${BUNDLE}/Contents/PkgInfo" "BNDL${_sig}")
file(COPY "${IMPORT_README}" DESTINATION "${BUNDLE}/Contents/Resources/Content/Import")
find_program(CODESIGN codesign)
if(CODESIGN)
  execute_process(COMMAND "${CODESIGN}" --force --sign - "${BUNDLE}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "codesign failed: ${_err}")
  endif()
else()
  message(WARNING "codesign not found: Apple Silicon hosts may refuse the unsigned plugin")
endif()
