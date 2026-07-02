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

// TODO: void gba_core_init(GbaCoreState* state)
// TODO: bool gba_core_load_rom(GbaCoreState* state, const uint8_t* data, size_t size)
// TODO: bool gba_core_load_bios(GbaCoreState* state, const uint8_t* data, size_t size)
// TODO: void gba_core_reset(GbaCoreState* state)
// TODO: void gba_core_run_frame(GbaCoreState* state)
// TODO: const uint32_t* gba_core_get_framebuffer(GbaCoreState* state)
// TODO: const int16_t* gba_core_get_audio_buffer(GbaCoreState* state)
// TODO: DingMemoryRegion gba_core_get_memory_region(GbaCoreState* state, ...)
// TODO: bool gba_core_save_state(GbaCoreState* state, uint8_t* out, size_t* out_size)
// TODO: bool gba_core_load_state(GbaCoreState* state, const uint8_t* data, size_t size)
// TODO: void gba_core_shutdown(GbaCoreState* state)
