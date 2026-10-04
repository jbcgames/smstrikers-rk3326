# Cross-compilation toolchain: aarch64-linux-gnu targeting GLIBC 2.17+ (ArkOS / handheld compatible)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Prefer clang cross-compilation
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET   aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)

set(CMAKE_SYSROOT /home/jbc/bionic-sysroot)

# Point at GCC 10 headers and libraries for C++20 stop_token / jthread support
set(GCC_10_DIR "/usr/lib/gcc-cross/aarch64-linux-gnu/10")
set(GCC_10_INC "/usr/aarch64-linux-gnu/include/c++/10")

set(CMAKE_CXX_FLAGS_INIT "-pthread -I/home/jbc/bionic-sysroot/include-overlay -I${GCC_10_INC} -I${GCC_10_INC}/aarch64-linux-gnu -I${GCC_10_INC}/backward -B${GCC_10_DIR}")
set(CMAKE_C_FLAGS_INIT "-pthread -B${GCC_10_DIR}")

# Use GNU cross-linker with bionic-sysroot and static libstdc++
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-fuse-ld=/usr/bin/aarch64-linux-gnu-ld -pthread -B${GCC_10_DIR} -L${GCC_10_DIR} -L/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -L/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=/usr/bin/aarch64-linux-gnu-ld -pthread -B${GCC_10_DIR} -L${GCC_10_DIR} -L/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -L/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=/usr/bin/aarch64-linux-gnu-ld -pthread -B${GCC_10_DIR} -L${GCC_10_DIR} -L/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -L/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,/home/jbc/bionic-sysroot/lib/aarch64-linux-gnu -static-libstdc++ -static-libgcc")

# Sysroot / find-root configuration
set(CMAKE_FIND_ROOT_PATH /home/jbc/bionic-sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# RPATH for portable deployment
set(CMAKE_BUILD_RPATH "$ORIGIN:$ORIGIN/libs.aarch64")
set(CMAKE_INSTALL_RPATH "$ORIGIN:$ORIGIN/libs.aarch64")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
