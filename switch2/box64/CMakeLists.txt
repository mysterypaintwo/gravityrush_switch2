# The embedded Box64 core (static library box64e) for the Gravity Rush ARM64 host.
# Same sources as Box64's WoW64 build (third_party/box64/wine/wow64/CMakeLists.txt), with
# os_embed.c and box64e.c in place of the Windows OS layer and driver. Box64 itself is patched
# for the embedded build (switch2/box64/patches, applied by switch2/box64/apply_patches.sh).
cmake_minimum_required(VERSION 3.16)
project(box64e C ASM)

set(BOX64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/box64")
set(BOX64E_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(INTERPRETER
    emu/x64run.c emu/x64run0f.c emu/x64run66.c emu/x64run660f.c emu/x64run66f20f.c
    emu/x64run66f30f.c emu/x64run66d9.c emu/x64run66dd.c emu/x64run66f0.c emu/x64rund8.c
    emu/x64rund9.c emu/x64runda.c emu/x64rundb.c emu/x64rundc.c emu/x64rundd.c emu/x64runde.c
    emu/x64rundf.c emu/x64runf0.c emu/x64runf20f.c emu/x64runf30f.c emu/x64runavx.c
    emu/x64runavx0f.c emu/x64runavx0f38.c emu/x64runavx660f.c emu/x64runavxf20f.c
    emu/x64runavxf30f.c emu/x64runavx660f38.c emu/x64runavx660f3a.c emu/x64runavxf20f38.c
    emu/x64runavxf30f38.c emu/x64runavxf20f3a.c emu/x64runavxf30f3a.c)
list(TRANSFORM INTERPRETER PREPEND "${BOX64_ROOT}/src/")

set(DYNAREC_ASM_NAMES arm64_prolog.S arm64_epilog.S arm64_next.S arm64_lock.S)
set(DYNAREC_ASM)
foreach(asm ${DYNAREC_ASM_NAMES})
    if(APPLE)
        # Box64's .S files are written for ELF; asm_to_macho.py adds the Mach-O symbol
        # underscores and @PAGE/@PAGEOFF relocations.
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/asm/${asm}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/asm"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/asm_to_macho.py"
                "${BOX64_ROOT}/src/dynarec/arm64/${asm}" "${CMAKE_CURRENT_BINARY_DIR}/asm/${asm}"
            DEPENDS "${BOX64_ROOT}/src/dynarec/arm64/${asm}" "${CMAKE_CURRENT_SOURCE_DIR}/asm_to_macho.py"
            VERBATIM)
        list(APPEND DYNAREC_ASM "${CMAKE_CURRENT_BINARY_DIR}/asm/${asm}")
    else()
        list(APPEND DYNAREC_ASM "${BOX64_ROOT}/src/dynarec/arm64/${asm}")
    endif()
endforeach()

set(DYNAREC_PASS
    dynarec_arm64_helper.c dynarec_arm64_emit_tests.c dynarec_arm64_emit_math.c
    dynarec_arm64_emit_logic.c dynarec_arm64_emit_shift.c dynarec_arm64_00.c dynarec_arm64_0f.c
    dynarec_arm64_66.c dynarec_arm64_d8.c dynarec_arm64_d9.c dynarec_arm64_da.c dynarec_arm64_db.c
    dynarec_arm64_dc.c dynarec_arm64_dd.c dynarec_arm64_de.c dynarec_arm64_df.c dynarec_arm64_f0.c
    dynarec_arm64_660f.c dynarec_arm64_66f20f.c dynarec_arm64_66f30f.c dynarec_arm64_66f0.c
    dynarec_arm64_f20f.c dynarec_arm64_f30f.c dynarec_arm64_avx.c dynarec_arm64_avx_0f.c
    dynarec_arm64_avx_0f38.c dynarec_arm64_avx_66_0f.c dynarec_arm64_avx_f2_0f.c
    dynarec_arm64_avx_f3_0f.c dynarec_arm64_avx_66_0f38.c dynarec_arm64_avx_66_0f3a.c
    dynarec_arm64_avx_f2_0f38.c dynarec_arm64_avx_f2_0f3a.c dynarec_arm64_avx_f3_0f38.c
    updateflags_arm64_pass.c)
list(TRANSFORM DYNAREC_PASS PREPEND "${BOX64_ROOT}/src/dynarec/arm64/")
list(APPEND DYNAREC_PASS "${BOX64_ROOT}/src/dynarec/dynarec_native_pass.c")

set(CORE
    build_info.c custommem.c
    dynarec/arm64/arm64_immenc.c dynarec/arm64/arm64_printer.c dynarec/arm64/dynarec_arm64_arch.c
    dynarec/arm64/dynarec_arm64_functions.c dynarec/arm64/dynarec_arm64_jmpnext.c
    dynarec/arm64/dynarec_arm64_consts.c dynarec/arm64/updateflags_arm64.c
    dynarec/dynablock.c dynarec/dynarec_native_functions.c dynarec/dynarec_native.c
    dynarec/dynarec.c dynarec/dynacache_reloc.c
    emu/x64compstrings.c emu/x64emu.c emu/x64primop.c emu/x64shaext.c emu/x64test.c
    emu/x64trace.c emu/x64run_private.c emu/x87emu_private.c
    os/backtrace.c os/hostext_common.c os/my_cpuid.c os/sysinfo.c
    tools/alternate.c tools/bitutils.c tools/env.c tools/dynacache.c tools/rbtree.c)
list(TRANSFORM CORE PREPEND "${BOX64_ROOT}/src/")

# Generated headers live in the build directory, not in third_party/box64.
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(WRITE "${GEN_DIR}/git_head.h" "#define GITREV \"v0.4.4-gr\"\n")
add_custom_command(
    OUTPUT "${GEN_DIR}/dynacache_hashes.h"
    COMMAND "${Python3_EXECUTABLE}" "${BOX64_ROOT}/tools/gen_dynacache_hashes.py"
        --root "${BOX64_ROOT}" --output "${GEN_DIR}/dynacache_hashes.h"
    DEPENDS ${DYNAREC_PASS} ${INTERPRETER}
    VERBATIM)
add_custom_target(box64e_generated DEPENDS "${GEN_DIR}/dynacache_hashes.h")

set(BOX64E_INCLUDES "${GEN_DIR}" "${BOX64E_DIR}" "${BOX64_ROOT}/src/include" "${BOX64_ROOT}/src")
if(NINTENDO_SWITCH)
    list(PREPEND BOX64E_INCLUDES "${BOX64E_DIR}/horizon")  # sys/mman.h and other newlib gaps
endif()
set(BOX64E_DEFINES DYNAREC ARM64 ARM_DYNAREC BOX64_EMBED NOALIGN STATICBUILD)
if(APPLE)
    list(APPEND BOX64E_DEFINES _DARWIN_C_SOURCE _XOPEN_SOURCE=700)
endif()
# -fno-strict-aliasing and the warnings Box64 trips on clang; -include gives every Box64
# source the embedded-host shims.
set(BOX64E_FLAGS -fno-strict-aliasing -fvisibility=hidden -include "${BOX64E_DIR}/embed_compat.h"
    -Wno-unused-command-line-argument -Wno-format -Wno-pointer-sign -Wno-deprecated-declarations
    -Wno-incompatible-pointer-types-discards-qualifiers -Wno-tautological-constant-out-of-range-compare
    -Wno-unknown-warning-option -Wno-int-conversion -Wno-macro-redefined -Wno-shift-op-parentheses
    -Wno-bitwise-op-parentheses -Wno-gnu-variable-sized-type-not-at-end)

function(box64e_object name)
    add_library(${name} OBJECT ${ARGN})
    target_include_directories(${name} PRIVATE ${BOX64E_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${BOX64E_DEFINES})
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:${BOX64E_FLAGS}>)
    add_dependencies(${name} box64e_generated)
endfunction()

foreach(step RANGE 3)
    box64e_object(box64e_pass${step} ${DYNAREC_PASS})
    target_compile_definitions(box64e_pass${step} PRIVATE STEP=${step})
endforeach()
box64e_object(box64e_test_interpreter ${INTERPRETER})
target_compile_definitions(box64e_test_interpreter PRIVATE TEST_INTERPRETER)

add_library(box64e STATIC
    ${CORE} ${INTERPRETER} ${DYNAREC_ASM}
    "${BOX64E_DIR}/os_embed.c" "${BOX64E_DIR}/box64e.c"
    $<$<BOOL:${NINTENDO_SWITCH}>:${BOX64E_DIR}/os_embed_horizon.c>
    $<$<PLATFORM_ID:Darwin>:${BOX64E_DIR}/box64e_jit_macos.c>
    $<$<BOOL:${NINTENDO_SWITCH}>:${BOX64E_DIR}/box64e_jit_horizon.c>
    $<TARGET_OBJECTS:box64e_pass0> $<TARGET_OBJECTS:box64e_pass1>
    $<TARGET_OBJECTS:box64e_pass2> $<TARGET_OBJECTS:box64e_pass3>
    $<TARGET_OBJECTS:box64e_test_interpreter>)
target_include_directories(box64e PRIVATE ${BOX64E_INCLUDES})
target_include_directories(box64e INTERFACE "${BOX64E_DIR}")
target_compile_definitions(box64e PRIVATE ${BOX64E_DEFINES})
target_compile_options(box64e PRIVATE $<$<COMPILE_LANGUAGE:C>:${BOX64E_FLAGS}>)
add_dependencies(box64e box64e_generated)

if(NINTENDO_SWITCH)
    # Horizon (Switch 2 under kaiHEN): the JIT goes through libkaihen's RO hooks.
    target_link_libraries(box64e INTERFACE kaihen)
endif()

# A first test: translate and run small hand-written x86-64 functions (tests/box64e_smoke.c).
add_executable(box64e_smoke "${BOX64E_DIR}/tests/box64e_smoke.c")
target_link_libraries(box64e_smoke PRIVATE box64e m)
# Runs one stretch of the game's code (read from your eboot.elf) to compare dynarec and interpreter.
if(NOT NINTENDO_SWITCH)
    add_executable(box64e_block "${BOX64E_DIR}/tests/box64e_block.c")
    target_link_libraries(box64e_block PRIVATE box64e m)
    # Every SSE/AVX form of the game, dynarec vs interpreter (tests/simd_forms.py, simd_compare.py).
    add_executable(box64e_simdcheck "${BOX64E_DIR}/tests/box64e_simdcheck.c")
    target_link_libraries(box64e_simdcheck PRIVATE box64e m)
endif()
if(NINTENDO_SWITCH)
    target_include_directories(box64e_smoke PRIVATE "${BOX64E_DIR}/horizon")
    nx_create_nro(box64e_smoke)
endif()
