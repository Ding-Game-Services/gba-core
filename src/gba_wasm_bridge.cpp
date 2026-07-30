// gba_wasm_bridge.cpp -- Emscripten-only glue, no core logic.
// Exists because two things the JS harness needs can't be done through
// gba_core.h's existing extern "C" functions alone:
//  1. Nothing exports sizeof(GbaCoreState), so JS has nowhere to malloc
//     an instance from without hardcoding a size that'll silently drift.
//  2. gba_core_get_memory_region hands back a DingMemoryRegion* struct --
//     JS can't safely read arbitrary struct fields out of wasm memory
//     without knowing exact layout/padding, so this exposes the fields
//     Cockpit actually needs as flat accessor calls instead.
//
// Not part of GBA_CORE_SOURCES in CMakeLists.txt -- only added to the
// Emscripten target, since native frontends linking gba_core as a static
// lib can just use sizeof(GbaCoreState) and DingMemoryRegion directly in
// C++.

#include "gba_core.h"
#include <cstdlib>

extern "C" {

GbaCoreState* gba_wasm_alloc_instance() {
    return (GbaCoreState*)std::malloc(sizeof(GbaCoreState));
}

void gba_wasm_free_instance(GbaCoreState* state) {
    std::free(state);
}

uint32_t gba_wasm_region_base(GbaCoreState* state, uint32_t index) {
    DingMemoryRegion region;
    if (!gba_core_get_memory_region(state, index, &region)) return 0;
    return region.base_addr;
}

uint32_t gba_wasm_region_size(GbaCoreState* state, uint32_t index) {
    DingMemoryRegion region;
    if (!gba_core_get_memory_region(state, index, &region)) return 0;
    return region.size;
}

uint8_t* gba_wasm_region_ptr(GbaCoreState* state, uint32_t index) {
    DingMemoryRegion region;
    if (!gba_core_get_memory_region(state, index, &region)) return nullptr;
    return (uint8_t*)region.ptr;
}

int gba_wasm_region_writable(GbaCoreState* state, uint32_t index) {
    DingMemoryRegion region;
    if (!gba_core_get_memory_region(state, index, &region)) return 0;
    return region.writable;
}

const char* gba_wasm_region_name(GbaCoreState* state, uint32_t index) {
    DingMemoryRegion region;
    if (!gba_core_get_memory_region(state, index, &region)) return "";
    return region.name;
}

// Sets KEYINPUT (0x04000130), active-low per GBATEK: bit=0 means pressed.
// Bits: 0=A 1=B 2=Select 3=Start 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
// `pressed_mask` uses the same bit layout but active-HIGH (1=pressed) --
// this function does the active-low inversion so the JS side doesn't have
// to think about it. Not routed through io_write_hook since KEYINPUT is
// hardware-driven, not a game-writable register; writing memory.io
// directly is correct here (nothing else owns this register's value, and
// io_read_hook has no case for 0x130 so the raw byte passes straight
// through on read).
void gba_wasm_set_keys(GbaCoreState* state, uint16_t pressed_mask) {
    uint16_t keyinput = (~pressed_mask) & 0x3FF;
    state->memory.io[0x130] = (uint8_t)(keyinput & 0xFF);
    state->memory.io[0x131] = (uint8_t)((keyinput >> 8) & 0xFF);
}

// ---- Debug taps -----------------------------------------------------
// Not part of the core's real API surface -- just enough live state for
// the browser harness to tell "CPU is stuck at a fixed PC" apart from
// "CPU is running fine but the render/IO path is broken", without a full
// register-dump/tracing facility. See gba_harness.cpp's original planned
// stuck-detection notes -- this covers the same worry from the JS side.

uint32_t gba_wasm_debug_pc(GbaCoreState* state) { return state->cpu.r[15]; }
uint32_t gba_wasm_debug_cpsr(GbaCoreState* state) { return state->cpu.cpsr; }
int gba_wasm_debug_thumb(GbaCoreState* state) { return state->cpu.thumb_mode ? 1 : 0; }
uint32_t gba_wasm_debug_reg(GbaCoreState* state, int index) {
    if (index < 0 || index > 15) return 0;
    return state->cpu.r[index];
}
uint16_t gba_wasm_debug_dispcnt(GbaCoreState* state) { return state->ppu.dispcnt; }
uint16_t gba_wasm_debug_dispstat(GbaCoreState* state) { return state->ppu.dispstat; }
uint16_t gba_wasm_debug_vcount(GbaCoreState* state) { return state->ppu.vcount; }
uint16_t gba_wasm_debug_ime(GbaCoreState* state) { return state->interrupts.ime ? 1 : 0; }
uint16_t gba_wasm_debug_ie(GbaCoreState* state) { return state->interrupts.ie; }
uint16_t gba_wasm_debug_if(GbaCoreState* state) { return state->interrupts.if_; }

// Generic peek, mainly for inspecting raw opcode bytes around a stuck PC
// by hand when the harness's stuck-detection fires -- goes through the
// normal read path (gba_mem_read32), so BIOS/ROM/RAM are all reachable.
uint32_t gba_wasm_debug_peek32(GbaCoreState* state, uint32_t addr) {
    return gba_mem_read32(&state->memory, addr);
}

} // extern "C"
