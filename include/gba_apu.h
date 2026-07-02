#ifndef GBA_APU_H
#define GBA_APU_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// GBA audio: two halves
//  - 4 legacy PSG channels (2 pulse, 1 wave, 1 noise) -- same family as
//    GB APU, should be portable conceptually from ding gb-core.
//  - 2 Direct Sound channels: DMA-fed 8-bit PCM FIFOs. DMA1/DMA2 refill
//    the FIFO when it runs low; a timer (Timer 0 or 1, selectable) sets
//    the sample playback rate. This is what most GBA games actually use
//    for music.
// All 6 channels mixed together in hardware, output resampled to 32.768kHz.

// TODO: struct GbaApuState
//   - 4x PSG channel state (reuse/adapt gb-core APU channel shapes)
//   - 2x DirectSound FIFO (small ring buffer, refilled by DMA)
//   - SOUNDCNT_L/H/X register mirrors, SOUNDBIAS

// TODO: gba_apu_init(...)
// TODO: gba_apu_step(...)             advance PSG channels by cycles
// TODO: gba_apu_fifo_push(...)        DMA writes PCM bytes into FIFO
// TODO: gba_apu_timer_tick(...)       timer overflow drains FIFO sample
// TODO: gba_apu_mix(...)              combine all 6 channels -> output buffer

#ifdef __cplusplus
}
#endif

#endif // GBA_APU_H
