#include "gba_apu.h"

// APU implementation
//
// Plan:
//  - gba_apu_init: zero channel state, clear FIFOs
//  - gba_apu_step: advance PSG channels (pulse/wave/noise) by elapsed
//    cycles, same shape as gb-core's APU step -- port/adapt from there
//  - gba_apu_fifo_push: DMA1/DMA2 write path calls this to load PCM
//    bytes into the Direct Sound FIFO when it's running low
//  - gba_apu_timer_tick: called on Timer 0/1 overflow (whichever is
//    selected per channel), pops one sample from FIFO to output
//  - gba_apu_mix: sum all 6 channels per SOUNDCNT_L/H routing, apply
//    SOUNDBIAS, resample to output rate

// TODO: void gba_apu_init(GbaApuState* apu)
// TODO: void gba_apu_step(GbaApuState* apu, uint32_t cycles)
// TODO: void gba_apu_fifo_push(GbaApuState* apu, int channel, uint8_t sample)
// TODO: void gba_apu_timer_tick(GbaApuState* apu, int timer_index)
// TODO: void gba_apu_mix(GbaApuState* apu, int16_t* out_buffer, uint32_t frames)
