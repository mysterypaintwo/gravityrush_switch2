# gr_host: the Gravity Rush ARM64 host (macOS development build). The PC port's runtime
# (src/runtime*.c) plus the ARM64 loader (gr_host.c) on the embedded Box64 core.
cmake_minimum_required(VERSION 3.16)
project(gr_host C ASM)

set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${PORT_ROOT}/switch2/box64" box64e)

# Homebrew's pkg-config: devkitPro's comes first on the PATH and only knows Switch libraries.
if(APPLE AND NOT PKG_CONFIG_EXECUTABLE AND EXISTS /opt/homebrew/bin/pkg-config)
    set(PKG_CONFIG_EXECUTABLE /opt/homebrew/bin/pkg-config)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(HOSTLIBS REQUIRED IMPORTED_TARGET sdl3 libavcodec libavutil libswscale)

file(GLOB RUNTIME_SOURCES "${PORT_ROOT}/src/runtime*.c")
file(GLOB ATRAC9_SOURCES "${PORT_ROOT}/third_party/LibAtrac9/C/src/*.c")

add_library(atrac9 STATIC ${ATRAC9_SOURCES})
target_compile_options(atrac9 PRIVATE -w)

add_executable(gr_host
    gr_host.c gpu_none.c gr_fiber_arm64.c gr_ctx_arm64.S
    $<$<PLATFORM_ID:Darwin>:host_compat_macos.c>
    ${RUNTIME_SOURCES})
target_include_directories(gr_host PRIVATE "${PORT_ROOT}" "${PORT_ROOT}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gr_host PRIVATE GR_ARM64 GR_FIBER_HOST)
set_property(TARGET gr_host PROPERTY C_STANDARD 11)
if(APPLE)
    target_compile_definitions(gr_host PRIVATE _DARWIN_C_SOURCE)
    set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/host_compat.h")
endif()
target_compile_options(gr_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(gr_host PRIVATE box64e atrac9 PkgConfig::HOSTLIBS m pthread)

# Tests: guest code written in C, built for x86-64 and run under Box64 (tests/extract_text.py).
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
