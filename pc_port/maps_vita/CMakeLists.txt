# =============================================================================
# PS Vita Map Overlay Modules (.suprx)
# =============================================================================
# Each map overlay is built as a loadable kernel module (.suprx), loaded at
# runtime via sceKernelLoadStartModule (see ../src/dll_loader.c). This is the
# Vita counterpart of ../maps/CMakeLists.txt (which builds .so/.dll for
# Linux/Windows via dlopen) -- same map source files, same SH_MAP_DLL define,
# different OS-level loading mechanism because Vita has no dlopen().
#
# TEST-BUILD SCOPE (see docs/vita_port.md): only the 3 maps below are wired
# up so far (map0_s00 is already compiled into the main exe and needs no
# module). Adding another map means:
#   1. Add its add_map_module() call below.
#   2. Re-run the symbol measurement (partial-link the map's .o files, `nm -u`
#      the result, diff against SilentHillPC's defined globals) and add any
#      newly-needed symbols to vita_export/map_api_exports.yml.
#   3. If the new symbol count pushes the single stub library close to
#      vita-libs-gen's practical per-module export ceiling (~1024, empirically
#      undocumented), split map_api_exports.yml into multiple libraries.
# =============================================================================

set(DECOMP_DIR "${CMAKE_SOURCE_DIR}/..")
set(MAP_SRC_DIR "${DECOMP_DIR}/src/maps")
set(SH_VITA_STUBLIB_DIR "${CMAKE_BINARY_DIR}/map_stub_lib")

set(MAP_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/psyq_compat"
    "${CMAKE_SOURCE_DIR}/src"
    "${DECOMP_DIR}/include"
    "${DECOMP_DIR}/include/decomp"
    "${CMAKE_SOURCE_DIR}/PsyCross/include/psx"
    "${CMAKE_SOURCE_DIR}/PsyCross/include"
)

set(MAP_COMMON_DEFS
    VER_USA
    SH_PC_PORT
    SKIP_ASM
    SH_MAP_DLL
)

# =============================================================================
# Function to create one map overlay .suprx target
# =============================================================================
function(add_map_module MAP_NAME MAP_DEFINE)
    set(MAP_DIR "${MAP_SRC_DIR}/${MAP_NAME}")
    if(NOT EXISTS "${MAP_DIR}")
        message(STATUS "Map module ${MAP_NAME}: source directory not found, skipping")
        return()
    endif()

    file(GLOB MAP_SOURCES "${MAP_DIR}/*.c")
    if(NOT MAP_SOURCES)
        message(STATUS "Map module ${MAP_NAME}: no source files, skipping")
        return()
    endif()

    # Same extracted-data file the desktop DLL build includes (cutscene voice
    # cmds, timers, camera waypoints, and several previously-"missing" shared
    # data symbols -- see vita_export/map_api_exports.yml's header comment).
    set(EXTRACTED_DATA "${CMAKE_SOURCE_DIR}/build_gen/extracted_data/${MAP_NAME}_extracted_data.c")
    if(EXISTS "${EXTRACTED_DATA}")
        list(APPEND MAP_SOURCES "${EXTRACTED_DATA}")
    endif()

    # Generate this map's module_start/module_stop glue + vita-elf-create
    # config from the shared templates (see module_glue.c.in).
    set(MODULE_GLUE "${CMAKE_CURRENT_BINARY_DIR}/${MAP_NAME}/module_glue.c")
    set(MODULE_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/${MAP_NAME}/module_config.yml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/module_glue.c.in" "${MODULE_GLUE}" @ONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/module_config.yml.in" "${MODULE_CONFIG}" @ONLY)
    list(APPEND MAP_SOURCES "${MODULE_GLUE}")

    add_executable(${MAP_NAME}_mod ${MAP_SOURCES})

    target_compile_definitions(${MAP_NAME}_mod PRIVATE
        ${MAP_COMMON_DEFS}
        ${MAP_DEFINE}
        SH_MAP_NAME=${MAP_NAME}
        "static_assert=_Static_assert"
    )
    target_include_directories(${MAP_NAME}_mod PRIVATE ${MAP_INCLUDE_DIRS})
    target_compile_options(${MAP_NAME}_mod PRIVATE
        -include "${CMAKE_SOURCE_DIR}/include/map_dll_fixups.h"
    )

    # Same warning demotions as the desktop map DLL build (see
    # ../maps/CMakeLists.txt for the full rationale) -- the decomp has many
    # not-yet-finalized signatures that GCC promotes to hard errors by
    # default. arm-vita-eabi-gcc tracks a recent-enough GCC that these need
    # the same treatment as the Linux (GCC >= 14) desktop path.
    target_compile_options(${MAP_NAME}_mod PRIVATE
        -Wno-incompatible-pointer-types
        -Wno-discarded-qualifiers
        -Wno-builtin-declaration-mismatch
        -Wno-int-to-pointer-cast
        -Wno-pointer-to-int-cast
        -Wno-sign-compare
        -Wno-missing-braces
        -Wno-unused-variable
        -Wno-unused-function
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-int>
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=declaration-missing-parameter-type>
        $<$<COMPILE_LANGUAGE:C>:-Wno-error=return-mismatch>
    )

    # Link against the stub import library generated from
    # vita_export/map_api_exports.yml (see ../CMakeLists.txt's map_stub_lib
    # target) -- this is how the module's references to game-code symbols
    # (Bgm_Update, g_GameWork, etc.) get resolved. Sony's own module loader
    # does the actual NID-based binding against the running exe at
    # sceKernelLoadStartModule time; this static lib only exists to satisfy
    # the ARM linker at BUILD time (its .S stubs contain no real code).
    target_link_libraries(${MAP_NAME}_mod PRIVATE
        "${SH_VITA_STUBLIB_DIR}/libSilentHillPC_stub.a"
    )
    add_dependencies(${MAP_NAME}_mod map_stub_lib)

    # vita-elf-create appends the SCE module-info/export/import tables at the
    # END of the RX (code) segment, in whatever gap remains before the next
    # 64KB-aligned segment boundary. map1_s00_mod's code segment landed only
    # ~800 bytes short of that gap ("Cannot allocate 8756 bytes... segment 1
    # overlaps") -- --gc-sections drops the unused libc/static-init code each
    # module pulls in but never calls, shrinking the code segment enough to
    # leave room. Harmless on the maps that already fit (map0_s01/s02).
    # --gc-sections would otherwise strip module_start/module_stop themselves:
    # nothing in the binary calls them (Sony's loader does, from the VELF's
    # export table, which is added AFTER this link step) -- so force-keep
    # them as GC roots the same way __attribute__((used)) would.
    #
    # vita-elf-create writes the SCE module-info/export/import tables into
    # whatever gap is left at the END of the code (RX) segment before the
    # next 64KB-aligned segment boundary ("Cannot allocate N bytes for SCE
    # data at end of segment 0"). That gap shrinks as a map needs more
    # imported symbols (a bigger NID import table) and/or its own code grows
    # -- both --gc-sections AND -Os (favor code size over speed; map overlay
    # logic is nowhere near hot enough for -O3 to matter) buy back headroom.
    target_compile_options(${MAP_NAME}_mod PRIVATE -ffunction-sections -fdata-sections -Os)
    target_link_options(${MAP_NAME}_mod PRIVATE
        "LINKER:--gc-sections"
        "LINKER:-u,module_start"
        "LINKER:-u,module_stop"
    )

    set_target_properties(${MAP_NAME}_mod PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/maps_vita_out"
    )

    # ELF -> VELF (module_start/module_stop as entrypoints, see
    # module_config.yml.in) -> .suprx, same pipeline as SilentHillPC's own
    # ELF -> VELF -> SELF except the config declares module entrypoints
    # instead of a process main, and vita-make-fself is invoked directly
    # (SUPRX modules aren't a process-image SELF, so vita_create_self's
    # process-image-flavored macro doesn't fit here).
    set(VELF_FILE "${CMAKE_BINARY_DIR}/maps_vita_out/${MAP_NAME}.velf")
    set(SUPRX_FILE "${CMAKE_BINARY_DIR}/maps_vita_out/${MAP_NAME}.suprx")
    add_custom_command(
        OUTPUT "${VELF_FILE}"
        COMMAND "${VITA_ELF_CREATE}" -e "${MODULE_CONFIG}"
                "$<TARGET_FILE:${MAP_NAME}_mod>" "${VELF_FILE}"
        DEPENDS ${MAP_NAME}_mod "${MODULE_CONFIG}"
        COMMENT "Converting ${MAP_NAME}_mod to Sony ELF (${MAP_NAME}.velf)"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SUPRX_FILE}"
        COMMAND "${VITA_MAKE_FSELF}" "${VELF_FILE}" "${SUPRX_FILE}"
        DEPENDS "${VELF_FILE}"
        COMMENT "Creating ${MAP_NAME}.suprx"
        VERBATIM
    )
    add_custom_target(${MAP_NAME}_suprx ALL DEPENDS "${SUPRX_FILE}")

    message(STATUS "Map module ${MAP_NAME}: .suprx target created")
endfunction()

# =============================================================================
# Register test-build map overlays
# =============================================================================
# map0_s00 is compiled into the main executable -- no module needed for it.
add_map_module(map0_s01 MAP0_S01)
add_map_module(map0_s02 MAP0_S02)
add_map_module(map1_s00 MAP1_S00)
