cmake_minimum_required(VERSION 3.20)
project(gba_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ding-core-sdk lives as a git submodule at sdk/ per project convention --
# provides ding_types.h, ding_core.h, ding_audio.h/.cpp, ding_savestate.h/.cpp,
# ding_md5.cpp, and the headless test-harness scaffolding shared across cores.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)

# ---------------------------------------------------------------------------
# Core sources -- no platform-specific code lives in these files (per the
# project's Ding Core API principles); platform differences are handled
# entirely below in the per-target sections.
# ---------------------------------------------------------------------------
set(GBA_CORE_SOURCES
    gba_bios.cpp
    gba_apu.cpp
    gba_interrupts.cpp
    gba_timers.cpp
    gba_cpu.cpp
    gba_cpu_arm.cpp
    gba_cpu_thumb.cpp
    gba_memory.cpp
    gba_dma.cpp
    gba_ppu.cpp
    gba_core.cpp
)

set(GBA_CORE_HEADERS
    gba_bios.h
    gba_apu.h
    gba_interrupts.h
    gba_timers.h
    gba_cpu.h
    gba_memory.h
    gba_dma.h
    gba_ppu.h
    gba_core.h
)

# SDK sources shared with every other Ding core.
set(SDK_SOURCES
    ${SDK_DIR}/ding_audio.cpp
    ${SDK_DIR}/ding_md5.cpp
    ${SDK_DIR}/ding_savestate.cpp
)

# ---------------------------------------------------------------------------
# WASM target (Emscripten) -- this is what the browser HTML harness loads.
# Produces gba_core.js (loader/glue) + gba_core.wasm.
#
# BIOS/ROM are NOT baked in or read from any path here -- per gba_bios.h,
# they're caller-supplied at runtime via gba_core_load_bios/load_rom. The
# JS harness copies uploaded file bytes into the WASM heap and calls those
# functions directly; there is nothing for CMake or Emscripten's virtual
# filesystem to do here, so it's explicitly disabled below.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    add_executable(gba_core ${GBA_CORE_SOURCES} ${SDK_SOURCES})
    target_include_directories(gba_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SDK_DIR}
    )

    # Lifecycle + accessor functions the JS harness needs to call directly.
    # All already declared extern "C" in gba_core.h, so names aren't mangled.
    set(GBA_EXPORTED_FUNCTIONS
        "_gba_core_init"
        "_gba_core_load_bios"
        "_gba_core_load_rom"
        "_gba_core_reset"
        "_gba_core_run_frame"
        "_gba_core_get_framebuffer"
        "_gba_core_get_audio_buffer"
        "_gba_core_get_memory_region_count"
        "_gba_core_get_memory_region"
        "_gba_core_save_state"
        "_gba_core_load_state"
        "_gba_core_shutdown"
        "_malloc"
        "_free"
    )
    string(REPLACE ";" "','" GBA_EXPORTED_FUNCTIONS_JSON "${GBA_EXPORTED_FUNCTIONS}")
    set(GBA_EXPORTED_FUNCTIONS_JSON "['${GBA_EXPORTED_FUNCTIONS_JSON}']")

    target_link_options(gba_core PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createGbaCoreModule"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=${GBA_EXPORTED_FUNCTIONS_JSON}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPU32','HEAP32']"
        "-sENVIRONMENT=web"
        "-sFORCE_FILESYSTEM=0"   # no BIOS/ROM path lives on any virtual FS -- see note above
        "-sASSERTIONS=1"
        "-sSTACK_SIZE=1048576"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Debug>:-g>"
    )
    set_target_properties(gba_core PROPERTIES
        OUTPUT_NAME "gba_core"
        SUFFIX ".js"
    )

# ---------------------------------------------------------------------------
# Native targets -- Windows desktop, Linux desktop, Steam Deck (SteamOS),
# ROG Ally (Windows), all x86_64 per project's target-platform list.
# Builds gba_core as a static lib, plus the CLI test harness.
#
# NOTE: gba_harness.cpp is still a TODO skeleton with no real logic and no
# main() defined -- this target will configure but FAIL TO LINK until that
# file is actually implemented. Left wired up (rather than commented out)
# so it starts working the moment the harness gets filled in, but flagging
# here so the failure isn't a surprise.
# ---------------------------------------------------------------------------
else()
    add_library(gba_core STATIC ${GBA_CORE_SOURCES} ${SDK_SOURCES})
    target_include_directories(gba_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SDK_DIR}
    )

    add_executable(gba_harness gba_harness.cpp)
    target_link_libraries(gba_harness PRIVATE gba_core)
endif()
