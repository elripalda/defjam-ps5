# [ps5 port] CMake toolchain for building Def Jam: Fight for NY as a native
# PS5 homebrew app, with the PS5 payload SDK (mihawk-99 fork, as pinned by
# ps5-plume-triangle).
#
#   cmake -S . -B build/ps5 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=ps5/cmake/ps5-toolchain.cmake \
#     -DPS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk

if(NOT PS5_PAYLOAD_SDK)
  set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
endif()
if(NOT PS5_PAYLOAD_SDK OR NOT EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
  message(FATAL_ERROR "Set PS5_PAYLOAD_SDK to the PS5 payload SDK root")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK)

include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)

# Zen 2 (the PS5 CPU); emulated TLS (the PS5 ABI).
set(PS5_COMMON_FLAGS
  "-march=znver2 -femulated-tls -fno-stack-protector -fno-plt -fdenormal-fp-math=ieee")
set(CMAKE_C_FLAGS_INIT "${PS5_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS5_COMMON_FLAGS} -fexperimental-library -frtti -fexceptions")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fexperimental-library")
