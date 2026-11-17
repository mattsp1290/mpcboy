# cmake -P script run after linking mpcboy_macho: builds mpcboy.vst from scratch and ad-hoc signs it.
# Inputs: BINARY, BUNDLE, PLIST, UID.
file(REMOVE_RECURSE "${BUNDLE}")
file(MAKE_DIRECTORY "${BUNDLE}/Contents/MacOS" "${BUNDLE}/Contents/Resources/Content/Import")
file(COPY "${BINARY}" DESTINATION "${BUNDLE}/Contents/MacOS")
file(COPY "${PLIST}" DESTINATION "${BUNDLE}/Contents")
string(SUBSTRING "${UID}????" 0 4 _sig)
file(WRITE "${BUNDLE}/Contents/PkgInfo" "BNDL${_sig}")
# Same text as scripts/package_mpc.py writes for the MPC folder.
file(WRITE "${BUNDLE}/Contents/Resources/Content/Import/README.txt"
"Put LSDJ 9.4.0 .sav files here (up to 64, directly in this folder, no sub-folders).
On the TABLE page pick one with Imp File, then tap Imp Go. It replaces all 16 tables
and 16 waves (the first 16 tables in the file, waves 0-15). Imp Undo restores the
previous content; Imp Rescan re-reads this folder after you add files.
")
find_program(CODESIGN codesign)
if(CODESIGN)
  execute_process(COMMAND "${CODESIGN}" --force --sign - "${BUNDLE}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "codesign failed: ${_err}")
  endif()
else()
  message(WARNING "codesign not found: Apple Silicon hosts may refuse the unsigned plugin")
endif()
