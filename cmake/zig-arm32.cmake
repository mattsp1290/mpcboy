# Cross toolchain: zig cc targeting arm32 (glibc 2.31 symbol baseline), as in the
# Akai SDK's tools/build.py. Compiler wrappers live in scripts/.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(MPCBOY_TARGET arm32 CACHE STRING "" FORCE)

get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER   "${_root}/scripts/zig-cc")
set(CMAKE_CXX_COMPILER "${_root}/scripts/zig-c++")
set(CMAKE_AR           "${_root}/scripts/zig-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${_root}/scripts/zig-ranlib" CACHE FILEPATH "" FORCE)

set(_flags "-target arm-linux-gnueabihf.2.31 -mcpu=cortex_a17")
set(CMAKE_C_FLAGS_INIT   "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_flags}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
