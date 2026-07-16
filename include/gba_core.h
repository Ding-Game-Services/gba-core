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
#include "ding_audio.h"
#include "ding_md5.h"
#include "ding_core.h" // DingMemoryRegion, used by gba_core_get_memory_region

#ifdef __cplusplus
extern "C" {
#endif

// Top-level GBA core -- implements the ding_core.h contract (fixed
// lifecycle, capability-based design) and owns/wires together every
// subsystem stubbed so far.

// One frame's worth of GBA audio at 32768 Hz / ~59.73 fps -- used to size
// the scratch conversion buffer in gba_core_run_frame (gba_apu_mix
// outputs int16, DingAudioBuffer wants float, so mixing goes through this
// scratch buffer before ding_audio_write converts+pushes it).
#define GBA_AUDIO_SAMPLES_PER_FRAME 548

typedef struct {
    GbaCpuState cpu;
    GbaMemory memory;
    GbaPpuState ppu;
    GbaApuState apu;
    GbaDmaState dma;
    GbaTimerState timers;
    GbaInterruptState interrupts;
    GbaBiosDescriptor bios;

    // Audio ring buffer per ding_audio.h convention -- storage is
    // core-owned (no dynamic allocation per that header's rules).
    float audio_ring_storage[DING_AUDIO_DEFAULT_CAPACITY * 2]; // stereo
    DingAudioBuffer audio_ring;

    // ADDED: ROM identity, filled in gba_core_load_rom via ding_md5.h.
    // Not in the original TODO list but the TODO comment there flagged
    // needing "a place to live on GbaCoreState" -- this is that place.
    u8 rom_md5[DING_MD5_SIZE];
} GbaCoreState;

// Lifecycle functions -- this is the per-instance, state-passing layer
// gba_core.cpp implements. A separate thin adapter (not written yet) is
// expected to wrap these into the global-function ding_core.h contract
// (ding_init/ding_run_frame/etc, which take no state pointer) around one
// module-level GbaCoreState instance -- same shape every other Ding core
// presumably uses to bridge its own internal API to that contract.

void gba_core_init(GbaCoreState* state);
bool gba_core_load_rom(GbaCoreState* state, const uint8_t* data, size_t size);
bool gba_core_load_bios(GbaCoreState* state, const uint8_t* data, size_t size);
bool gba_core_reset(GbaCoreState* state);
void gba_core_run_frame(GbaCoreState* state);
const uint32_t* gba_core_get_framebuffer(GbaCoreState* state);

// Drains available mixed audio from state->audio_ring into out_buf
// (interleaved stereo floats, per ding_audio.h). Returns the number of
// sample frames actually copied -- caller should size out_buf for at
// least `max_frames * 2` floats.
uint32_t gba_core_get_audio_buffer(GbaCoreState* state, float* out_buf, uint32_t max_frames);

// Fills `out` with the memory region at `index`. Returns false if index
// is out of range. See gba_core_get_memory_region_count for the count.
uint32_t gba_core_get_memory_region_count();
bool gba_core_get_memory_region(GbaCoreState* state, uint32_t index, DingMemoryRegion* out);

// .ding format (ding_savestate.h). Returns bytes written / DING_OK on
// success, mirroring ding_save_state/ding_load_state's own conventions.
size_t gba_core_save_state(GbaCoreState* state, uint8_t* out, size_t out_size);
DingResult gba_core_load_state(GbaCoreState* state, const uint8_t* data, size_t size);

void gba_core_shutdown(GbaCoreState* state);

#ifdef __cplusplus
}
#endif

#endif // GBA_CORE_H
