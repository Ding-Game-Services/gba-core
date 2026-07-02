#ifndef GBA_CORE_H
#define GBA_CORE_H

#include <cstdint>
#include "gba_cpu.h"
#include "gba_memory.h"
#include "gba_ppu.h"
#include "gba_apu.h"
#include "gba_dma.h"
#include "gba_timers.h"
#include "gba_interrupts.h"
#include "gba_bios.h"

#ifdef __cplusplus
extern "C" {
#endif

// Top-level GBA core -- implements the ding_core.h contract (fixed
// lifecycle, capability-based design) and owns/wires together every
// subsystem stubbed so far.

// TODO: struct GbaCoreState
//   - GbaCpuState cpu
//   - GbaMemory memory
//   - GbaPpuState ppu
//   - GbaApuState apu
//   - GbaDmaState dma
//   - GbaTimerState timers
//   - GbaInterruptState interrupts
//   - GbaBiosDescriptor bios

// TODO: ding_core lifecycle functions (per ding_core.h contract):
//   - gba_core_init(...)
//   - gba_core_load_rom(...)      MD5 hash per RA spec, save-type detect
//   - gba_core_load_bios(...)     wraps gba_bios_load
//   - gba_core_reset(...)
//   - gba_core_run_frame(...)     main loop: cpu step -> ppu/apu/timers/dma
//                                 tick -> interrupt check, until VBlank
//   - gba_core_get_framebuffer(...)
//   - gba_core_get_audio_buffer(...)
//   - gba_core_get_memory_region(...) -> DingMemoryRegion, for
//                                        Cockpit/engine achievement reads
//   - gba_core_save_state / gba_core_load_state (.ding format)
//   - gba_core_shutdown(...)

#ifdef __cplusplus
}
#endif

#endif // GBA_CORE_H
