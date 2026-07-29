#include "gba_apu.h"
#include <cstring>

// APU implementation
//
// SCOPE / APPROXIMATIONS FOR THIS PASS (flagging explicitly, matching the
// stub's own admission that this is a standalone first pass not yet
// reconciled with gb-core):
//  - Square/wave/noise frequency-to-period math is the classic GB formula
//    scaled 4x for the GBA's 16.78MHz system clock. Not verified against
//    real hardware timing yet.
//  - No frequency sweep on square1 (register field isn't in
//    GbaPsgSquareChannel at all yet -- flag if sweep is needed soon).
//  - gba_apu_mix reads each channel's *currently held* digital output
//    level for every sample in the requested buffer, rather than
//    resynthesizing per-sample inside mix itself. This only sounds right
//    if gba_apu_step is called at fine cycle granularity throughout the
//    frame (so output_high/wave sample_pos actually advance in between
//    mix calls) -- calling step once then mixing a whole frame at once
//    will sound wrong. Caller (gba_core_run_frame) needs to interleave
//    step/mix at sub-frame granularity once CPU-side timing exists.
//  - gba_apu_timer_tick's header comment says it should signal the owning
//    DMA channel to refill via gba_dma_trigger -- this function has no
//    GbaDmaState* parameter to do that with, so that signal is left to
//    the caller (check whether the popped FIFO count dropped to <=16 and
//    call gba_dma_trigger(..., GBA_DMA_TIMING_SPECIAL) itself). Same
//    signature-gap shape as gba_dma_trigger's missing GbaMemory*.

#define GBA_CLOCK_HZ 16777216u
#define GBA_ENVELOPE_PERIOD_CYCLES (GBA_CLOCK_HZ / 64u)   // 64 Hz envelope clock
#define GBA_LENGTH_PERIOD_CYCLES   (GBA_CLOCK_HZ / 256u)  // 256 Hz length clock
#define GBA_SWEEP_PERIOD_CYCLES    (GBA_CLOCK_HZ / 128u)  // 128 Hz sweep clock (channel 1 only)

static const uint8_t GBA_DUTY_PATTERNS[4] = {
    0b00000001, // 12.5%
    0b00000011, // 25%
    0b00001111, // 50%
    0b11111100, // 75%
};

static const uint16_t GBA_NOISE_DIVISORS[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

void gba_apu_init(GbaApuState* apu) {
    std::memset(apu, 0, sizeof(GbaApuState));
    apu->noise.lfsr = 0x7FFF; // all-1s seed, standard GB/GBA noise reset state
}

// ---- Envelope / length helpers (shared shape across all 3 PSG types) --

static void gba_apu_step_envelope(uint8_t* volume, uint8_t envelope_step, bool increase,
                                   uint32_t* accum, uint32_t cycles) {
    if (envelope_step == 0) return; // step 0 = envelope disabled, per GBATEK
    *accum += cycles;
    uint32_t period = GBA_ENVELOPE_PERIOD_CYCLES * envelope_step;
    while (*accum >= period) {
        *accum -= period;
        if (increase && *volume < 15) (*volume)++;
        else if (!increase && *volume > 0) (*volume)--;
    }
}

static void gba_apu_step_length(bool length_enable, uint8_t* length_counter, bool* enabled,
                                 uint32_t* accum, uint32_t cycles) {
    if (!length_enable || *length_counter == 0) return;
    *accum += cycles;
    while (*accum >= GBA_LENGTH_PERIOD_CYCLES) {
        *accum -= GBA_LENGTH_PERIOD_CYCLES;
        if (*length_counter > 0) {
            (*length_counter)--;
            if (*length_counter == 0) *enabled = false;
        }
    }
}

// ---- Per-channel stepping ----------------------------------------------

static void gba_apu_step_square(GbaPsgSquareChannel* ch, uint32_t cycles) {
    if (!ch->enabled) return;

    // GB-style period, scaled 4x for GBA clock (see file-top note).
    uint32_t period = (2048u - ch->frequency_reg) * 16u;
    if (period == 0) period = 16u;

    ch->freq_timer_accum += cycles;
    while (ch->freq_timer_accum >= period) {
        ch->freq_timer_accum -= period;
        ch->duty_pos = (ch->duty_pos + 1) & 0x7;
        uint8_t pattern = GBA_DUTY_PATTERNS[ch->duty & 0x3];
        ch->output_high = (pattern >> ch->duty_pos) & 1;
    }

gba_apu_step_envelope(&ch->volume, ch->envelope_step, ch->envelope_increase,
                           &ch->envelope_accum, cycles);
    gba_apu_step_length(ch->length_enable, &ch->length_counter, &ch->enabled,
                         &ch->length_accum, cycles);
}

// ADDED: frequency sweep, channel 1 only (see struct-top note in
// gba_apu.h). Classic GB sweep algorithm: periodically compute a new
// frequency from the shadow register, disable the channel on overflow
// past the 11-bit frequency range. NOTE: real hardware also does an
// immediate overflow-check sweep calculation at trigger time (not just
// periodically) -- not modeled here, a rare edge case affecting only
// sweep configs that would overflow on the very first calculation.
static void gba_apu_step_sweep(GbaPsgSquareChannel* ch, uint32_t cycles) {
    if (!ch->enabled || ch->sweep_period == 0) return;

    ch->sweep_accum += cycles;
    uint32_t period = GBA_SWEEP_PERIOD_CYCLES * ch->sweep_period;
    while (ch->sweep_accum >= period) {
        ch->sweep_accum -= period;
        if (ch->sweep_shift == 0) continue; // timer reloads, no frequency change

        uint16_t delta = ch->sweep_shadow_freq >> ch->sweep_shift;
        int32_t new_freq = ch->sweep_negate
            ? (int32_t)ch->sweep_shadow_freq - (int32_t)delta
            : (int32_t)ch->sweep_shadow_freq + (int32_t)delta;

        if (new_freq > 2047 || new_freq < 0) {
            ch->enabled = false; // overflow disables the channel
        } else {
            ch->sweep_shadow_freq = (uint16_t)new_freq;
            ch->frequency_reg = (uint16_t)new_freq;
        }
    }
}

static void gba_apu_step_wave(GbaPsgWaveChannel* ch, uint32_t cycles) {
    if (!ch->enabled) return;

    // Wave channel steps through 32 samples per period (vs 8 duty steps
    // for square), hence the finer /2 relative to the square formula.
    uint32_t period = (2048u - ch->frequency_reg) * 8u;
    if (period == 0) period = 8u;

    ch->freq_timer_accum += cycles;
    while (ch->freq_timer_accum >= period) {
        ch->freq_timer_accum -= period;
        ch->sample_pos = (ch->sample_pos + 1) & 0x1F;
    }

    gba_apu_step_length(ch->length_enable, &ch->length_counter, &ch->enabled,
                         &ch->length_accum, cycles);
}

static void gba_apu_step_noise(GbaPsgNoiseChannel* ch, uint32_t cycles) {
    if (!ch->enabled) return;

    uint32_t divisor = GBA_NOISE_DIVISORS[ch->divisor_code & 0x7];
    uint32_t period = divisor << (ch->clock_shift + 1);
    period *= 4u; // scale to GBA clock, see file-top note
    if (period == 0) period = 4u;

    ch->freq_timer_accum += cycles;
    while (ch->freq_timer_accum >= period) {
        ch->freq_timer_accum -= period;

        // 15-bit (or 7-bit narrow-mode) Galois LFSR, standard GB/GBA noise.
        uint16_t bit = (ch->lfsr & 1) ^ ((ch->lfsr >> 1) & 1);
        ch->lfsr >>= 1;
        ch->lfsr |= bit << 14;
        if (ch->narrow_mode) {
            ch->lfsr = (ch->lfsr & ~(1u << 6)) | (bit << 6);
        }
        ch->output_high = (ch->lfsr & 1) == 0;
    }

    gba_apu_step_envelope(&ch->volume, ch->envelope_step, ch->envelope_increase,
                           &ch->envelope_accum, cycles);
    gba_apu_step_length(ch->length_enable, &ch->length_counter, &ch->enabled,
                         &ch->length_accum, cycles);
}

void gba_apu_step(GbaApuState* apu, uint32_t cycles) {
    gba_apu_step_sweep(&apu->square1, cycles);
    gba_apu_step_square(&apu->square1, cycles);
    gba_apu_step_square(&apu->square2, cycles);
    gba_apu_step_wave(&apu->wave, cycles);
    gba_apu_step_noise(&apu->noise, cycles);
}

// ---- Direct Sound FIFOs -------------------------------------------------

void gba_apu_fifo_push(GbaApuState* apu, int fifo_index, const int8_t* data, uint8_t count) {
    GbaDirectSoundFifo* fifo = (fifo_index == 0) ? &apu->fifo_a : &apu->fifo_b;

    for (uint8_t i = 0; i < count; i++) {
        if (fifo->count >= GBA_DIRECTSOUND_FIFO_CAPACITY) break; // full, drop remainder
        fifo->samples[fifo->write_pos] = data[i];
        fifo->write_pos = (fifo->write_pos + 1) % GBA_DIRECTSOUND_FIFO_CAPACITY;
        fifo->count++;
    }
}

void gba_apu_timer_tick(GbaApuState* apu, int fifo_index) {
    GbaDirectSoundFifo* fifo = (fifo_index == 0) ? &apu->fifo_a : &apu->fifo_b;
    if (fifo->count == 0) return; // nothing queued -- real HW repeats last sample; skipped for now

    fifo->read_pos = (fifo->read_pos + 1) % GBA_DIRECTSOUND_FIFO_CAPACITY;
    fifo->count--;
    // Refill-DMA signal left to caller once count drops low -- see
    // file-top note on gba_apu_timer_tick's missing GbaDmaState* param.
}

// ---- Mixing ---------------------------------------------------------

void gba_apu_mix(GbaApuState* apu, int16_t* out_buffer, uint32_t sample_count) {
    // PSG channels: held digital state * volume, held constant across
    // this whole call -- see file-top note on why (no per-sample
    // resynthesis here; caller must step() at fine granularity instead).
    int16_t square1_level = apu->square1.enabled && apu->square1.output_high
        ? (int16_t)(apu->square1.volume * 8) : 0;
    int16_t square2_level = apu->square2.enabled && apu->square2.output_high
        ? (int16_t)(apu->square2.volume * 8) : 0;

    int16_t wave_level = 0;
    if (apu->wave.enabled && apu->wave.volume_shift > 0) {
        uint8_t byte = apu->wave.wave_ram[apu->wave.sample_pos / 2];
        uint8_t nibble = (apu->wave.sample_pos & 1) ? (byte & 0xF) : (byte >> 4);
        int16_t signed_sample = (int16_t)nibble - 8; // center 4-bit sample
        wave_level = (int16_t)(signed_sample * (16 >> apu->wave.volume_shift));
    }

    int16_t noise_level = apu->noise.enabled && apu->noise.output_high
        ? (int16_t)(apu->noise.volume * 8) : 0;

    // ---- Panning / volume (SOUNDCNT_L, SOUNDCNT_H) ----------------------
    // SOUNDCNT_L: bits 0-2 PSG master volume RIGHT, bits 4-6 PSG master
    // volume LEFT (0-7 -> (vol+1)/8 scale per GBATEK), bits 8-11 per-PSG-
    // channel RIGHT enable (sq1,sq2,wave,noise), bits 12-15 same for LEFT.
    bool sq1_r   = (apu->soundcnt_l >> 8)  & 1;
    bool sq2_r   = (apu->soundcnt_l >> 9)  & 1;
    bool wave_r  = (apu->soundcnt_l >> 10) & 1;
    bool noise_r = (apu->soundcnt_l >> 11) & 1;
    bool sq1_l   = (apu->soundcnt_l >> 12) & 1;
    bool sq2_l   = (apu->soundcnt_l >> 13) & 1;
    bool wave_l  = (apu->soundcnt_l >> 14) & 1;
    bool noise_l = (apu->soundcnt_l >> 15) & 1;

    uint32_t psg_vol_r = apu->soundcnt_l & 0x7;
    uint32_t psg_vol_l = (apu->soundcnt_l >> 4) & 0x7;

    int32_t psg_right = (sq1_r ? square1_level : 0) + (sq2_r ? square2_level : 0)
                       + (wave_r ? wave_level : 0)   + (noise_r ? noise_level : 0);
    int32_t psg_left  = (sq1_l ? square1_level : 0) + (sq2_l ? square2_level : 0)
                       + (wave_l ? wave_level : 0)   + (noise_l ? noise_level : 0);

    psg_right = psg_right * (int32_t)(psg_vol_r + 1) / 8;
    psg_left  = psg_left  * (int32_t)(psg_vol_l + 1) / 8;

    // SOUNDCNT_H bits 0-1: overall PSG volume ratio (0=25% 1=50% 2=100%,
    // 3 is prohibited per GBATEK -- treated as 100% here rather than
    // trapping on it).
    uint32_t psg_ratio = apu->soundcnt_h & 0x3;
    int32_t psg_num = (psg_ratio == 0) ? 1 : (psg_ratio == 1) ? 2 : 4;
    psg_right = psg_right * psg_num / 4;
    psg_left  = psg_left  * psg_num / 4;

    // SOUNDCNT_H bits 2-3: Direct Sound A/B volume (0=50% 1=100%).
    // bits 8-9: Direct Sound A right/left enable. bits 12-13: same for B.
    bool dsa_vol_full = (apu->soundcnt_h >> 2) & 1;
    bool dsb_vol_full = (apu->soundcnt_h >> 3) & 1;
    bool dsa_r = (apu->soundcnt_h >> 8)  & 1;
    bool dsa_l = (apu->soundcnt_h >> 9)  & 1;
    bool dsb_r = (apu->soundcnt_h >> 12) & 1;
    bool dsb_l = (apu->soundcnt_h >> 13) & 1;

    // Direct Sound: pop whatever's queued, hold last popped sample steady
    // when a FIFO runs dry (matches real hardware's "repeat last sample"
    // behavior on underrun).
    for (uint32_t i = 0; i < sample_count; i++) {
        int8_t fifo_a_sample = apu->fifo_a.last_sample;
        if (apu->fifo_a.count > 0) {
            fifo_a_sample = apu->fifo_a.samples[apu->fifo_a.read_pos];
            apu->fifo_a.last_sample = fifo_a_sample;
        }
        int8_t fifo_b_sample = apu->fifo_b.last_sample;
        if (apu->fifo_b.count > 0) {
            fifo_b_sample = apu->fifo_b.samples[apu->fifo_b.read_pos];
            apu->fifo_b.last_sample = fifo_b_sample;
        }

        // Base scale of 2 (50%) / 4 (100%) keeps roughly the same overall
        // amplitude as the old flat "*4 combined" approximation while now
        // respecting the volume bit.
        int32_t a_val = (int32_t)fifo_a_sample * (dsa_vol_full ? 4 : 2);
        int32_t b_val = (int32_t)fifo_b_sample * (dsb_vol_full ? 4 : 2);

        int32_t right = psg_right + (dsa_r ? a_val : 0) + (dsb_r ? b_val : 0);
        int32_t left  = psg_left  + (dsa_l ? a_val : 0) + (dsb_l ? b_val : 0);

        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;

        out_buffer[i * 2 + 0] = (int16_t)left;
        out_buffer[i * 2 + 1] = (int16_t)right;
    }
}
