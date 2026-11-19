# gr_host: the Gravity Rush ARM64 host. The PC port's runtime (src/runtime*.c) plus the ARM64
# loader (gr_host.c) on the embedded Box64 core. macOS: the development build (out/gr_host);
# Switch 2 (cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/libounce/cmake/Switch2.cmake): the console
# NRO, gravityrush_remastered.nro (out/gr_switch).
cmake_minimum_required(VERSION 3.16)
project(gr_host C CXX ASM)

set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${PORT_ROOT}/switch2/box64" box64e)

# The GPU library (shadPS4 video core + shim, gpu/): Vulkan on MoltenVK on the Mac.
# GR_HOST_GPU=OFF builds without it (gpu_none.c): the game then stops at its first GPU import.
option(GR_HOST_GPU "Build with the GPU library" ON)
if(GR_HOST_GPU)
    set(GOW3_LTO OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PORT_ROOT}/gpu" gpu)
endif()

if(NINTENDO_SWITCH)
    # FFmpeg (the game's movies and MP3/AT9 decoding) from the Switch portlibs, which the
    # toolchain searches after the Switch 2 ones; no SDL (gr_sdl_horizon.h, gr_audio_horizon.h).
    find_library(AVCODEC avcodec REQUIRED)
    find_library(AVUTIL avutil REQUIRED)
    find_library(SWSCALE swscale REQUIRED)
    find_library(SWRESAMPLE swresample)
    add_library(hostlibs INTERFACE)
    find_library(DAV1D dav1d REQUIRED)
    find_library(ZLIB z REQUIRED)
    target_link_libraries(hostlibs INTERFACE ${SWSCALE} ${AVCODEC} ${AVUTIL} $<$<BOOL:${SWRESAMPLE}>:${SWRESAMPLE}>
                          ${DAV1D} ${ZLIB})
    target_include_directories(hostlibs INTERFACE "${DEVKITPRO}/portlibs/switch/include")
else()
    # Homebrew's pkg-config: devkitPro's comes first on the PATH and only knows Switch libraries.
    if(APPLE AND NOT PKG_CONFIG_EXECUTABLE AND EXISTS /opt/homebrew/bin/pkg-config)
        set(PKG_CONFIG_EXECUTABLE /opt/homebrew/bin/pkg-config)
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HOSTLIBS REQUIRED IMPORTED_TARGET sdl3 libavcodec libavutil libswscale)
    add_library(hostlibs INTERFACE)
    target_link_libraries(hostlibs INTERFACE PkgConfig::HOSTLIBS)
endif()

file(GLOB RUNTIME_SOURCES "${PORT_ROOT}/src/runtime*.c")
file(GLOB ATRAC9_SOURCES "${PORT_ROOT}/third_party/LibAtrac9/C/src/*.c")

add_library(atrac9 STATIC ${ATRAC9_SOURCES})
target_compile_options(atrac9 PRIVATE -w)

add_executable(gr_host
    gr_host.c gr_fiber_arm64.c gr_ctx_arm64.S $<$<NOT:$<BOOL:${GR_HOST_GPU}>>:gpu_none.c>
    $<$<PLATFORM_ID:Darwin>:host_compat_macos.c>
    $<$<BOOL:${NINTENDO_SWITCH}>:gr_switch.c gr_suspend_horizon.c gr_heap_horizon.c host_compat_horizon.c>
    ${RUNTIME_SOURCES})
target_include_directories(gr_host PRIVATE "${PORT_ROOT}" "${PORT_ROOT}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gr_host PRIVATE GR_ARM64 GR_FIBER_HOST)
set_property(TARGET gr_host PROPERTY C_STANDARD 11)
if(APPLE)
    target_compile_definitions(gr_host PRIVATE _DARWIN_C_SOURCE)
endif()
if(APPLE OR NINTENDO_SWITCH)
    set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/host_compat.h")
endif()
if(NINTENDO_SWITCH)
    # sys/mman.h, dlfcn.h, execinfo.h (Box64's Horizon shims) and sys/random.h (horizon/).
    target_include_directories(gr_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/horizon" "${PORT_ROOT}/switch2/box64/horizon")
    # Every thread registers itself so HOME / sleep can freeze it (gr_suspend_horizon.c).
    target_link_options(gr_host PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=_Exit
        -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_cond_timedwait -Wl,--wrap=sched_yield -Wl,--wrap=pthread_join -Wl,--wrap=nanosleep -Wl,--wrap=abort -Wl,--wrap=pthread_detach
        # Bring-up: who holds the host heap (gr_heap_horizon.c).
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=aligned_alloc -Wl,--wrap=memalign -Wl,--wrap=posix_memalign)
    set_target_properties(gr_host PROPERTIES OUTPUT_NAME gravityrush_remastered)
    nx_generate_nacp(gravityrush_remastered.nacp NAME "Gravity Rush Remastered" AUTHOR "mysterypaint" VERSION "0.1")
    nx_create_nro(gr_host NACP gravityrush_remastered.nacp)
endif()
target_compile_options(gr_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(gr_host PRIVATE box64e atrac9 hostlibs m)
if(NOT NINTENDO_SWITCH)
    target_link_libraries(gr_host PRIVATE pthread)
endif()
if(GR_HOST_GPU)
    if(APPLE)
        target_link_directories(gr_host PRIVATE /opt/homebrew/lib)  # Zycore (Zydis' dependency)
    endif()
    target_link_libraries(gr_host PRIVATE gow3gpu)
    target_compile_definitions(gr_host PRIVATE GR_HOST_GPU)
endif()

# Tests (development host only): guest code written in C, built for x86-64 and run under Box64
# (tests/extract_text.py).
if(NINTENDO_SWITCH)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(guest_blob name)
    set(obj "${CMAKE_CURRENT_BINARY_DIR}/${name}.o")
    set(hdr "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.h")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_C_COMPILER}" -target x86_64-unknown-linux-gnu -O1 -ffreestanding -fno-stack-protector
                -fPIC -mno-red-zone -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.c" -o "${obj}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/extract_text.py" "${obj}" "${hdr}" ${name}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.c" "${CMAKE_CURRENT_SOURCE_DIR}/tests/extract_text.py"
        VERBATIM)
endfunction()
guest_blob(fiber_guest)
add_executable(fiber_test tests/fiber_test.c gr_fiber_arm64.c gr_ctx_arm64.S
    "${CMAKE_CURRENT_BINARY_DIR}/generated/fiber_guest.h")
target_include_directories(fiber_test PRIVATE "${PORT_ROOT}/src" "${PORT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(fiber_test PRIVATE GR_ARM64)
target_link_libraries(fiber_test PRIVATE box64e m pthread)
