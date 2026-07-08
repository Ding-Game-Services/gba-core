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

// PSG channel shapes below are a standalone first pass, NOT yet reconciled
// against gb-core's actual APU channel structs -- the file's original plan
// called for reuse/adaptation from there. Revisit once gb-core's shapes
// are in front of us, since sharing code between the two cores was the
// point.

typedef struct {
    uint16_t frequency_reg;   // raw NRxx-style frequency/period bits
    uint8_t  duty;             // square wave duty cycle selector
    uint8_t  volume;
    uint8_t  envelope_step;
    bool     envelope_increase;
    bool     length_enable;
    uint8_t  length_counter;
    bool     enabled;
} GbaPsgSquareChannel;

typedef struct {
    uint8_t  wave_ram[16];     // 32 x 4-bit samples
    uint8_t  volume_shift;     // 0=mute,1=100%,2=50%,3=25% per GBATEK
    uint16_t frequency_reg;
    bool     length_enable;
    uint8_t  length_counter;
    bool     enabled;
} GbaPsgWaveChannel;

typedef struct {
    uint8_t  volume;
    uint8_t  envelope_step;
    bool     envelope_increase;
    uint8_t  clock_shift;
    uint8_t  divisor_code;
    bool     narrow_mode;      // width of the LFSR (15-bit vs 7-bit)
    bool     length_enable;
    uint8_t  length_counter;
    bool     enabled;
} GbaPsgNoiseChannel;

#define GBA_DIRECTSOUND_FIFO_CAPACITY 32 // bytes; hardware FIFO is 32 bytes deep

typedef struct {
    int8_t   samples[GBA_DIRECTSOUND_FIFO_CAPACITY];
    uint8_t  read_pos;
    uint8_t  write_pos;
    uint8_t  count;            // bytes currently queued
} GbaDirectSoundFifo;

typedef struct {
    GbaPsgSquareChannel square1;
    GbaPsgSquareChannel square2;
    GbaPsgWaveChannel   wave;
    GbaPsgNoiseChannel  noise;

    GbaDirectSoundFifo fifo_a; // Direct Sound A (typically Timer 0-driven)
    GbaDirectSoundFifo fifo_b; // Direct Sound B (typically Timer 1-driven)

    // Register mirrors
    uint16_t soundcnt_l; // PSG volume/panning
    uint16_t soundcnt_h; // Direct Sound volume/routing/timer select
    uint16_t soundcnt_x; // master enable + channel-active status bits
    uint16_t soundbias;
} GbaApuState;

void gba_apu_init(GbaApuState* apu);

// Advances the 4 PSG channels by `cycles` (envelope/length/sweep timing,
// waveform generation). Direct Sound channels are event-driven instead
// (see fifo_push/timer_tick below), not stepped here.
void gba_apu_step(GbaApuState* apu, uint32_t cycles);

// DMA1/2 call this on FIFO-empty/Special-timing refill to push PCM bytes
// into the named channel's ring buffer. `fifo_index` is 0 for A, 1 for B.
void gba_apu_fifo_push(GbaApuState* apu, int fifo_index, const int8_t* data, uint8_t count);

// Called when the Timer selected in SOUNDCNT_H for this FIFO overflows --
// pops one sample and (if it drops below half-full) should signal the
// owning DMA channel to refill, which the caller wires up via gba_dma_trigger.
void gba_apu_timer_tick(GbaApuState* apu, int fifo_index);

// Mixes all 6 channels into an interleaved stereo output buffer of
// `sample_count` frames.
void gba_apu_mix(GbaApuState* apu, int16_t* out_buffer, uint32_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // GBA_APU_H
