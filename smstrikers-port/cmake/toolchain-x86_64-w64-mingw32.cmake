# Cross-compilation toolchain: x86_64-w64-windows-gnu (Clang for Windows x86-64)
#
# Usage:
#   cmake -S . -B build/windows-x86_64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-w64-mingw32.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Compilers: Clang with Windows-GNU target
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Includes for MinGW C++20 standard library (std::jthread, std::stop_token)
set(MINGW_SYSROOT "/usr/x86_64-w64-mingw32")
set(MINGW_GCC_LIB "/usr/lib/gcc/x86_64-w64-mingw32/10-posix")
set(MINGW_CXX_INC "${MINGW_GCC_LIB}/include/c++")

set(CMAKE_C_FLAGS_INIT "--target=x86_64-w64-windows-gnu -O3 -DNDEBUG -DTARGET_PC=1 -fsigned-char -I${MINGW_SYSROOT}/include")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-w64-windows-gnu -O3 -DNDEBUG -DTARGET_PC=1 -fsigned-char -I${MINGW_CXX_INC} -I${MINGW_CXX_INC}/x86_64-w64-mingw32 -I${MINGW_CXX_INC}/backward -I${MINGW_SYSROOT}/include")

# Linker options
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=/usr/bin/x86_64-w64-mingw32-ld -L${MINGW_GCC_LIB} -lpthread -Wl,--allow-multiple-definition")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=/usr/bin/x86_64-w64-mingw32-ld -L${MINGW_GCC_LIB} -lpthread -Wl,--allow-multiple-definition")

# Sysroot / search paths
set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT} ${MINGW_GCC_LIB})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Map imported configurations so Release artifacts can be used in RelWithDebInfo / MinSizeRel
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release RelWithDebInfo "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release MinSizeRel "")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Debug Release "")
