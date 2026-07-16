#include "gba_core.h"

// Top-level core implementation -- wires cpu/memory/ppu/apu/dma/timers/
// interrupts/bios together and exposes the ding_core.h lifecycle.
//
// Main loop shape (gba_core_run_frame):
//   while (not vblank reached):
//     gba_interrupts_check(&state.interrupts) -> maybe gba_cpu_enter_exception
//     gba_cpu_step(&state.cpu, &state.memory)   [dispatches arm/thumb]
//     advance timers/dma/ppu by elapsed cycles from that step
//   gba_ppu_render_frame(&state.ppu, &state.memory)  [per-frame, see gba_ppu.h]
//
// Everything here is a thin coordinator -- actual logic lives in the
// per-subsystem files already scaffolded.

// One-time setup after construction. Does NOT boot the CPU -- no BIOS or
// ROM is loaded yet at this point, so there's nothing valid to execute.
// Safe to call exactly once; gba_core_reset is the repeatable entry point.
void gba_core_init(GbaCoreState* state) {
    gba_cpu_init(&state->cpu);
    gba_interrupts_init(&state->interrupts);

    // No ROM/BIOS bytes yet -- gba_mem_init is called again in
    // gba_core_reset once both are loaded, so this just zeroes the
    // memory arrays via a null/zero-size init.
    gba_mem_init(&state->memory, nullptr, nullptr, 0);

    state->bios.data     = nullptr;
    state->bios.size     = 0;
    state->bios.is_valid = false;

    // TODO: gba_ppu_init/gba_apu_init/gba_dma_init/gba_timers_init once
    // those headers are in hand -- not wired yet.
}

// Wraps gba_bios_load + validates. Caller-owned buffer, not copied,
// matching GbaMemory::rom convention.
bool gba_core_load_bios(GbaCoreState* state, const uint8_t* data, size_t size) {
    if (!gba_bios_load(&state->bios, data, size)) {
        return false;
    }
    return gba_bios_validate(&state->bios);
}

// TODO: MD5 hash per RA spec (ROM ID) not yet computed/stored here --
// needs a place to live on GbaCoreState once achievement/leaderboard
// hookup happens. Save-type detection is wired below.
bool gba_core_load_rom(GbaCoreState* state, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    state->memory.rom      = data;
    state->memory.rom_size = (uint32_t)size;

    GbaSaveType save_type = gba_mem_detect_save_type(data, (uint32_t)size);
    state->memory.save_type = save_type;

    // TODO: allocate state->memory.save_data based on save_type once the
    // per-type sizes are pinned down (SRAM 32KB, EEPROM 512B/8KB, Flash
    // 64KB/128KB) -- deferred, not needed until save read/write opcodes
    // in gba_memory.cpp are implemented.
    state->memory.save_data = nullptr;
    state->memory.save_size = 0;

    return true;
}

// Repeatable boot entry point -- call after load_bios/load_rom, or again
// to reset a running game. Requires a valid BIOS: per gba_bios.h, HLE is
// deferred, so there is no fallback boot path without a real dump.
bool gba_core_reset(GbaCoreState* state) {
    if (!state->bios.is_valid) {
        return false;
    }

    gba_cpu_init(&state->cpu);
    gba_interrupts_init(&state->interrupts);
    gba_mem_init(&state->memory, state->bios.data, state->memory.rom, state->memory.rom_size);

    // Real hardware boots executing BIOS from 0x00000000 in Supervisor
    // mode, IRQ/FIQ disabled (I and F bits set), ARM state (not Thumb).
    state->cpu.current_mode = GBA_MODE_SUPERVISOR;
    state->cpu.cpsr = (uint32_t)GBA_MODE_SUPERVISOR | CPSR_I_BIT | CPSR_F_BIT;
    state->cpu.thumb_mode = false;
    state->cpu.r[15] = 0x00000000;

    // TODO: gba_ppu_reset/gba_apu_reset/gba_dma_reset/gba_timers_reset
    // once those headers exist -- PPU scanline/VCOUNT state in particular
    // needs to be zeroed here or run_frame's VBlank wait will be wrong.

    return true;
}

// TODO: void gba_core_run_frame(GbaCoreState* state)
// TODO: const uint32_t* gba_core_get_framebuffer(GbaCoreState* state)
// TODO: const int16_t* gba_core_get_audio_buffer(GbaCoreState* state)
// TODO: DingMemoryRegion gba_core_get_memory_region(GbaCoreState* state, ...)
// TODO: bool gba_core_save_state(GbaCoreState* state, uint8_t* out, size_t* out_size)
// TODO: bool gba_core_load_state(GbaCoreState* state, const uint8_t* data, size_t size)
// TODO: void gba_core_shutdown(GbaCoreState* state)
