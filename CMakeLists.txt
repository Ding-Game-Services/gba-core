cmake_minimum_required(VERSION 3.20)
project(gba_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ding-core-sdk lives as a git submodule at sdk/ per project convention --
# provides ding_types.h, ding_core.h, ding_audio.h/.cpp, ding_savestate.h/.cpp,
# ding_md5.cpp, and the headless test-harness scaffolding shared across cores.
# SDK mirrors this repo's own src/+include/ split.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)

# ---------------------------------------------------------------------------
# Core sources -- no platform-specific code lives in these files (per the
# project's Ding Core API principles); platform differences are handled
# entirely below in the per-target sections.
# ---------------------------------------------------------------------------
set(GBA_CORE_SOURCES
    src/gba_bios.cpp
    src/gba_apu.cpp
    src/gba_interrupts.cpp
    src/gba_timers.cpp
    src/gba_cpu.cpp
    src/gba_cpu_arm.cpp
    src/gba_cpu_thumb.cpp
    src/gba_memory.cpp
    src/gba_dma.cpp
    src/gba_ppu.cpp
    src/gba_core.cpp
)

set(GBA_CORE_HEADERS
    include/gba_bios.h
    include/gba_apu.h
    include/gba_interrupts.h
    include/gba_timers.h
    include/gba_cpu.h
    include/gba_memory.h
    include/gba_dma.h
    include/gba_ppu.h
    include/gba_core.h
)

# SDK sources shared with every other Ding core.
set(SDK_SOURCES
    ${SDK_DIR}/src/ding_audio.cpp
    ${SDK_DIR}/src/ding_md5.cpp
    ${SDK_DIR}/src/ding_savestate.cpp
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
    add_executable(gba_core ${GBA_CORE_SOURCES} ${SDK_SOURCES} src/gba_wasm_bridge.cpp)
    target_include_directories(gba_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SDK_DIR}/include
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
        "_gba_wasm_alloc_instance"
        "_gba_wasm_free_instance"
        "_gba_wasm_region_base"
        "_gba_wasm_region_size"
        "_gba_wasm_region_ptr"
        "_gba_wasm_region_writable"
        "_gba_wasm_region_name"
        "_gba_wasm_set_keys"
        "_gba_wasm_debug_pc"
        "_gba_wasm_debug_cpsr"
        "_gba_wasm_debug_thumb"
        "_gba_wasm_debug_reg"
        "_gba_wasm_debug_dispcnt"
        "_gba_wasm_debug_dispstat"
        "_gba_wasm_debug_vcount"
        "_gba_wasm_debug_ime"
        "_gba_wasm_debug_ie"
        "_gba_wasm_debug_if"
        "_gba_wasm_debug_peek32"
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
# Builds gba_core as a shared library (.dll/.so) -- RetroArch-style core
# loading, matching the other Ding cores, rather than a static lib linked
# straight into the frontend.
#
# WINDOWS_EXPORT_ALL_SYMBOLS: MSVC does not auto-export symbols from a DLL
# the way GCC/Clang do for a .so -- without this the DLL builds but every
# gba_core_*/gba_wasm_* function is inaccessible to whatever loads it.
# Exporting at the build-system level means no __declspec(dllexport)/
# dllimport macros need to be added to the core headers themselves (keeps
# core files platform-agnostic per the project's Ding Core API principles).
# Linux builds use CMake's normal default SHARED suffix (.so) -- no
# override needed there.
#
# No CLI test harness here -- the browser HTML page is the real test
# harness for this core (see project notes). gba_harness.cpp is unused.
# ---------------------------------------------------------------------------
else()
    add_library(gba_core SHARED ${GBA_CORE_SOURCES} ${SDK_SOURCES})
    target_include_directories(gba_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SDK_DIR}/include
    )
    if(WIN32)
        set_target_properties(gba_core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
endif()
