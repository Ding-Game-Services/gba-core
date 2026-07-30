#include "gba_core.h"
#include "ding_savestate.h"

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
// ADDED: IO write dispatcher. gba_memory.h's io_write_hook mechanism
// existed but nothing ever set it, so every game write to DISPCNT/BGxCNT/
// DMAxCNT/TMxCNT/SOUNDCNT/etc only updated the flat mem->io[] byte buffer
// and never reached the subsystem that actually cares. This routes each
// known register address to the right subsystem call/field.
// ADDED: read-side counterpart to gba_core_io_write_hook. Covers the
// registers whose value changes from live hardware activity rather than
// game writes -- see gba_memory.h's io_read_hook comment. Everything not
// listed here just passes raw_value through untouched (i.e. whatever the
// game last wrote there), which is correct for pure control registers.
static uint16_t gba_core_io_read_hook(void* context, uint32_t addr, uint16_t raw_value) {
    GbaCoreState* state = (GbaCoreState*)context;
    switch (addr) {
        // DISPSTAT: ppu.dispstat is already the authoritative live value
        // (status bits set directly in gba_core_run_frame, IRQ-enable
        // bits set via the write hook's writable_mask) -- ignore
        // raw_value's mem->io[] mirror entirely rather than merging.
        case 0x04000004: return state->ppu.dispstat;
        case 0x04000006: return state->ppu.vcount; // VCOUNT: live scanline, read-only on real hardware
        case 0x04000202: return state->interrupts.if_; // IF: set by gba_interrupts_request, bypasses mem->io[]
        case 0x04000100: return state->timers.timers[0].counter;
        case 0x04000104: return state->timers.timers[1].counter;
        case 0x04000108: return state->timers.timers[2].counter;
        case 0x0400010C: return state->timers.timers[3].counter;
        case 0x04000084: { // SOUNDCNT_X: bits 0-3 are live per-channel active status
            uint16_t status = 0;
            if (state->apu.square1.enabled) status |= 0x1;
            if (state->apu.square2.enabled) status |= 0x2;
            if (state->apu.wave.enabled)    status |= 0x4;
            if (state->apu.noise.enabled)   status |= 0x8;
            return (uint16_t)((state->apu.soundcnt_x & 0xFF80) | status);
        }
        default:
            return raw_value;
    }
}

static void gba_core_io_write_hook(void* context, uint32_t addr, uint16_t value) {
    GbaCoreState* state = (GbaCoreState*)context;
    switch (addr) {
        // ---- PPU: DISPCNT / DISPSTAT ----
        case 0x04000000: state->ppu.dispcnt = value; break;
        case 0x04000004: {
            // Only bits 3-7 (IRQ enables + vcount-match setting) are
            // writable; bits 0-2 (VBlank/HBlank/VCounter status flags)
            // are set by the PPU itself in gba_core_run_frame -- preserve
            // them instead of letting a game's write clobber them.
            uint16_t writable_mask = 0xFFF8;
            state->ppu.dispstat = (state->ppu.dispstat & ~writable_mask) | (value & writable_mask);
            break;
        }
        // BGxCNT
        case 0x04000008: state->ppu.bg[0].control = value; break;
        case 0x0400000A: state->ppu.bg[1].control = value; break;
        case 0x0400000C: state->ppu.bg[2].control = value; break;
        case 0x0400000E: state->ppu.bg[3].control = value; break;
        // BGxHOFS/VOFS (write-only, 9-bit)
        case 0x04000010: state->ppu.bg[0].scroll_x = value & 0x1FF; break;
        case 0x04000012: state->ppu.bg[0].scroll_y = value & 0x1FF; break;
        case 0x04000014: state->ppu.bg[1].scroll_x = value & 0x1FF; break;
        case 0x04000016: state->ppu.bg[1].scroll_y = value & 0x1FF; break;
        case 0x04000018: state->ppu.bg[2].scroll_x = value & 0x1FF; break;
        case 0x0400001A: state->ppu.bg[2].scroll_y = value & 0x1FF; break;
        case 0x0400001C: state->ppu.bg[3].scroll_x = value & 0x1FF; break;
        case 0x0400001E: state->ppu.bg[3].scroll_y = value & 0x1FF; break;
        // BG2/BG3 affine PA-PD (8.8 fixed point, fits int16 as-is)
        case 0x04000020: state->ppu.affine[0].pa = (int16_t)value; break;
        case 0x04000022: state->ppu.affine[0].pb = (int16_t)value; break;
        case 0x04000024: state->ppu.affine[0].pc = (int16_t)value; break;
        case 0x04000026: state->ppu.affine[0].pd = (int16_t)value; break;
        case 0x04000030: state->ppu.affine[1].pa = (int16_t)value; break;
        case 0x04000032: state->ppu.affine[1].pb = (int16_t)value; break;
        case 0x04000034: state->ppu.affine[1].pc = (int16_t)value; break;
        case 0x04000036: state->ppu.affine[1].pd = (int16_t)value; break;
// BG2X/Y, BG3X/Y -- 28-bit signed 20.8 fixed-point affine
        // reference points. Now that GbaAffineParams::ref_x/ref_y is
        // int32_t (see gba_ppu.h), accumulate lo/hi halves the same way
        // DMA SAD/DAD do, then sign-extend from bit 27 once the high
        // half lands (bits 31-28 are unused/undefined on real hardware).
        case 0x04000028: // BG2X_L
            state->ppu.affine[0].ref_x = (int32_t)(((uint32_t)state->ppu.affine[0].ref_x & 0xFFFF0000u) | value);
            break;
        case 0x0400002A: { // BG2X_H
            uint32_t raw = ((uint32_t)state->ppu.affine[0].ref_x & 0x0000FFFFu) | ((uint32_t)value << 16);
            state->ppu.affine[0].ref_x = ((int32_t)(raw << 4)) >> 4; // sign-extend 28-bit field
            break;
        }
        case 0x0400002C: // BG2Y_L
            state->ppu.affine[0].ref_y = (int32_t)(((uint32_t)state->ppu.affine[0].ref_y & 0xFFFF0000u) | value);
            break;
        case 0x0400002E: { // BG2Y_H
            uint32_t raw = ((uint32_t)state->ppu.affine[0].ref_y & 0x0000FFFFu) | ((uint32_t)value << 16);
            state->ppu.affine[0].ref_y = ((int32_t)(raw << 4)) >> 4;
            break;
        }
        case 0x04000038: // BG3X_L
            state->ppu.affine[1].ref_x = (int32_t)(((uint32_t)state->ppu.affine[1].ref_x & 0xFFFF0000u) | value);
            break;
        case 0x0400003A: { // BG3X_H
            uint32_t raw = ((uint32_t)state->ppu.affine[1].ref_x & 0x0000FFFFu) | ((uint32_t)value << 16);
            state->ppu.affine[1].ref_x = ((int32_t)(raw << 4)) >> 4;
            break;
        }
        case 0x0400003C: // BG3Y_L
            state->ppu.affine[1].ref_y = (int32_t)(((uint32_t)state->ppu.affine[1].ref_y & 0xFFFF0000u) | value);
            break;
case 0x0400003E: { // BG3Y_H
            uint32_t raw = ((uint32_t)state->ppu.affine[1].ref_y & 0x0000FFFFu) | ((uint32_t)value << 16);
            state->ppu.affine[1].ref_y = ((int32_t)(raw << 4)) >> 4;
            break;
        }

        // ---- Windows ----
        case 0x04000040: state->ppu.win0h = value; break;
        case 0x04000042: state->ppu.win1h = value; break;
        case 0x04000044: state->ppu.win0v = value; break;
case 0x04000048: state->ppu.winin = value; break;
        case 0x0400004A: state->ppu.winout = value; break;

        // ---- Color special effects ----
        case 0x04000050: state->ppu.bldcnt = value; break;
        case 0x04000052: state->ppu.bldalpha = value; break;
        case 0x04000054: state->ppu.bldy = value; break;

        case 0x0400004C: state->ppu.mosaic = value; break;

        // ---- DMA0-3: SAD/DAD (32-bit, written as lo/hi halfwords) + CNT ----
        case 0x040000B0: state->dma.channels[0].src_addr = (state->dma.channels[0].src_addr & 0xFFFF0000u) | value; break;
        case 0x040000B2: state->dma.channels[0].src_addr = (state->dma.channels[0].src_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000B4: state->dma.channels[0].dst_addr = (state->dma.channels[0].dst_addr & 0xFFFF0000u) | value; break;
        case 0x040000B6: state->dma.channels[0].dst_addr = (state->dma.channels[0].dst_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000B8: state->dma.channels[0].word_count = value & 0x3FFF; break; // 14-bit on DMA0-2
        case 0x040000BA: gba_dma_write_control(&state->dma, 0, value); break;

        case 0x040000BC: state->dma.channels[1].src_addr = (state->dma.channels[1].src_addr & 0xFFFF0000u) | value; break;
        case 0x040000BE: state->dma.channels[1].src_addr = (state->dma.channels[1].src_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000C0: state->dma.channels[1].dst_addr = (state->dma.channels[1].dst_addr & 0xFFFF0000u) | value; break;
        case 0x040000C2: state->dma.channels[1].dst_addr = (state->dma.channels[1].dst_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000C4: state->dma.channels[1].word_count = value & 0x3FFF; break;
        case 0x040000C6: gba_dma_write_control(&state->dma, 1, value); break;

        case 0x040000C8: state->dma.channels[2].src_addr = (state->dma.channels[2].src_addr & 0xFFFF0000u) | value; break;
        case 0x040000CA: state->dma.channels[2].src_addr = (state->dma.channels[2].src_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000CC: state->dma.channels[2].dst_addr = (state->dma.channels[2].dst_addr & 0xFFFF0000u) | value; break;
        case 0x040000CE: state->dma.channels[2].dst_addr = (state->dma.channels[2].dst_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000D0: state->dma.channels[2].word_count = value & 0x3FFF; break;
        case 0x040000D2: gba_dma_write_control(&state->dma, 2, value); break;

        case 0x040000D4: state->dma.channels[3].src_addr = (state->dma.channels[3].src_addr & 0xFFFF0000u) | value; break;
        case 0x040000D6: state->dma.channels[3].src_addr = (state->dma.channels[3].src_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000D8: state->dma.channels[3].dst_addr = (state->dma.channels[3].dst_addr & 0xFFFF0000u) | value; break;
        case 0x040000DA: state->dma.channels[3].dst_addr = (state->dma.channels[3].dst_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x040000DC: state->dma.channels[3].word_count = value; break; // DMA3 gets full 16 bits
        case 0x040000DE: gba_dma_write_control(&state->dma, 3, value); break;

        // ---- Timers ----
        case 0x04000100: gba_timers_set_reload(&state->timers, 0, value); break;
        case 0x04000102: gba_timers_write_control(&state->timers, 0, value); break;
        case 0x04000104: gba_timers_set_reload(&state->timers, 1, value); break;
        case 0x04000106: gba_timers_write_control(&state->timers, 1, value); break;
        case 0x04000108: gba_timers_set_reload(&state->timers, 2, value); break;
        case 0x0400010A: gba_timers_write_control(&state->timers, 2, value); break;
        case 0x0400010C: gba_timers_set_reload(&state->timers, 3, value); break;
        case 0x0400010E: gba_timers_write_control(&state->timers, 3, value); break;

        // ---- Interrupts ----
        case 0x04000200: state->interrupts.ie = value; break;
        case 0x04000202: gba_interrupts_ack(&state->interrupts, value); break;
        case 0x04000208: state->interrupts.ime = (value & 0x1) != 0; break;

// ---- APU: PSG channel 1 (square + sweep) ----
        case 0x04000060: // SOUND1CNT_L
            state->apu.square1.sweep_shift  = value & 0x7;
            state->apu.square1.sweep_negate = (value >> 3) & 0x1;
            state->apu.square1.sweep_period = (value >> 4) & 0x7;
            break;
        case 0x04000062: // SOUND1CNT_H
            state->apu.square1.length_counter    = 64 - (value & 0x3F);
            state->apu.square1.duty              = (value >> 6) & 0x3;
            state->apu.square1.envelope_step     = (value >> 8) & 0x7;
            state->apu.square1.envelope_increase = (value >> 11) & 0x1;
            state->apu.square1.volume            = (value >> 12) & 0xF;
            break;
        case 0x04000064: // SOUND1CNT_X
            state->apu.square1.frequency_reg = value & 0x7FF;
            state->apu.square1.length_enable = (value >> 14) & 0x1;
            if ((value >> 15) & 0x1) { // restart/trigger bit
                state->apu.square1.enabled = true;
                state->apu.square1.duty_pos = 0;
                state->apu.square1.freq_timer_accum = 0;
                // Sweep shadow register reloads from the current
                // frequency on every trigger, and its timer restarts.
                state->apu.square1.sweep_shadow_freq = state->apu.square1.frequency_reg;
                state->apu.square1.sweep_accum = 0;
            }
            break;

        // ---- APU: PSG channel 2 (square, no sweep) ----
        case 0x04000068: // SOUND2CNT_L
            state->apu.square2.length_counter    = 64 - (value & 0x3F);
            state->apu.square2.duty              = (value >> 6) & 0x3;
            state->apu.square2.envelope_step     = (value >> 8) & 0x7;
            state->apu.square2.envelope_increase = (value >> 11) & 0x1;
            state->apu.square2.volume            = (value >> 12) & 0xF;
            break;
        case 0x0400006C: // SOUND2CNT_H
            state->apu.square2.frequency_reg = value & 0x7FF;
            state->apu.square2.length_enable = (value >> 14) & 0x1;
            if ((value >> 15) & 0x1) {
                state->apu.square2.enabled = true;
                state->apu.square2.duty_pos = 0;
                state->apu.square2.freq_timer_accum = 0;
            }
            break;

        // ---- APU: PSG channel 3 (wave) ----
        case 0x04000070: // SOUND3CNT_L
            // TODO: bit5 (wave RAM dual-bank select) not modeled --
            // GbaPsgWaveChannel::wave_ram is a single 16-byte bank. Bit7
            // (DAC power) folded into `enabled` for now.
            state->apu.wave.enabled = (value >> 7) & 0x1;
            break;
        case 0x04000072: // SOUND3CNT_H
            state->apu.wave.length_counter = 256 - (value & 0xFF);
            state->apu.wave.volume_shift   = (value >> 13) & 0x3;
            break;
        case 0x04000074: // SOUND3CNT_X
            state->apu.wave.frequency_reg = value & 0x7FF;
            state->apu.wave.length_enable = (value >> 14) & 0x1;
            if ((value >> 15) & 0x1) {
                state->apu.wave.enabled = true;
                state->apu.wave.sample_pos = 0;
                state->apu.wave.freq_timer_accum = 0;
            }
            break;

        // ---- APU: PSG channel 4 (noise) ----
        case 0x04000078: // SOUND4CNT_L
            state->apu.noise.length_counter    = 64 - (value & 0x3F);
            state->apu.noise.envelope_step     = (value >> 8) & 0x7;
            state->apu.noise.envelope_increase = (value >> 11) & 0x1;
            state->apu.noise.volume            = (value >> 12) & 0xF;
            break;
        case 0x0400007C: // SOUND4CNT_H
            state->apu.noise.divisor_code  = value & 0x7;
            state->apu.noise.narrow_mode   = (value >> 3) & 0x1;
            state->apu.noise.clock_shift   = (value >> 4) & 0xF;
            state->apu.noise.length_enable = (value >> 14) & 0x1;
            if ((value >> 15) & 0x1) {
                state->apu.noise.enabled = true;
                state->apu.noise.lfsr = 0x7FFF; // standard reset seed
            }
            break;

        // ---- APU: master control ----
        case 0x04000080: state->apu.soundcnt_l = value; break;
        case 0x04000082: {
            state->apu.soundcnt_h = value;
            // Bits 11/15 are write-only FIFO reset triggers, not stored
            // state -- clear the ring buffer immediately on write.
            if ((value >> 11) & 0x1) {
                state->apu.fifo_a.read_pos = 0;
                state->apu.fifo_a.write_pos = 0;
                state->apu.fifo_a.count = 0;
            }
            if ((value >> 15) & 0x1) {
                state->apu.fifo_b.read_pos = 0;
                state->apu.fifo_b.write_pos = 0;
                state->apu.fifo_b.count = 0;
            }
            break;
        }
        case 0x04000084:
            // SOUNDCNT_X -- only bit7 (master enable) is actually
            // writable per GBATEK, bits 0-3 are read-only per-channel
            // active status. Storing the whole value for now since
            // nothing reads soundcnt_x's status bits back yet -- TODO
            // once per-channel active-status reads are needed.
            state->apu.soundcnt_x = value;
            break;
        case 0x04000088: state->apu.soundbias = value; break;

        // Wave RAM (16 bytes, 8 halfword registers)
        case 0x04000090: case 0x04000092: case 0x04000094: case 0x04000096:
        case 0x04000098: case 0x0400009A: case 0x0400009C: case 0x0400009E: {
            uint32_t off = addr - 0x04000090;
            state->apu.wave.wave_ram[off]     = (uint8_t)(value & 0xFF);
            state->apu.wave.wave_ram[off + 1] = (uint8_t)((value >> 8) & 0xFF);
            break;
        }

        // FIFO A/B push registers -- a 32-bit write decomposes into two
        // 16-bit writes here (see gba_memory.cpp), so each halfword push
        // 2 signed-byte samples to reconstruct the same 4-byte push.
        case 0x040000A0: case 0x040000A2: {
            int8_t bytes[2] = { (int8_t)(value & 0xFF), (int8_t)((value >> 8) & 0xFF) };
            gba_apu_fifo_push(&state->apu, 0, bytes, 2);
            break;
        }
        case 0x040000A4: case 0x040000A6: {
            int8_t bytes[2] = { (int8_t)(value & 0xFF), (int8_t)((value >> 8) & 0xFF) };
            gba_apu_fifo_push(&state->apu, 1, bytes, 2);
            break;
        }

        default:
            // Unhandled register. Value is still stored in the flat
            // mem->io[] buffer by gba_mem_write16 itself, so reads won't
            // be totally wrong -- just inert until a real handler lands.
            break;
    }
}

void gba_core_init(GbaCoreState* state) {
    gba_cpu_init(&state->cpu);
    gba_interrupts_init(&state->interrupts);

    // No ROM/BIOS bytes yet -- gba_mem_init is called again in
    // gba_core_reset once both are loaded, so this just zeroes the
    // memory arrays via a null/zero-size init.
    gba_mem_init(&state->memory, nullptr, nullptr, 0);
    state->memory.io_hook_context = state;
    state->memory.io_write_hook = gba_core_io_write_hook;
    state->memory.io_read_hook_context = state;
    state->memory.io_read_hook = gba_core_io_read_hook;

    state->bios.data     = nullptr;
    state->bios.size     = 0;
    state->bios.is_valid = false;

    gba_ppu_init(&state->ppu);
    gba_apu_init(&state->apu);
    gba_dma_init(&state->dma);
    gba_timers_init(&state->timers);

    ding_audio_init(&state->audio_ring, state->audio_ring_storage,
                     DING_AUDIO_DEFAULT_CAPACITY, 2, 32768);
}

// Wraps gba_bios_load + validates. Caller-owned buffer, not copied,
// matching GbaMemory::rom convention.
bool gba_core_load_bios(GbaCoreState* state, const uint8_t* data, size_t size) {
    if (!gba_bios_load(&state->bios, data, size)) {
        return false;
    }
    return gba_bios_validate(&state->bios);
}

// ROM ID per RA spec (MD5, full-file per DingIdentityMethod's
// DING_ID_MD5_FULL -- GBA has no copier-header stripping concern like
// NES/SNES, so full-file is correct here). Save-type detection below.
bool gba_core_load_rom(GbaCoreState* state, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    state->memory.rom      = data;
    state->memory.rom_size = (uint32_t)size;

    ding_md5(data, size, state->rom_md5);

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
    state->memory.io_hook_context = state;
    state->memory.io_write_hook = gba_core_io_write_hook;
    state->memory.io_read_hook_context = state;
    state->memory.io_read_hook = gba_core_io_read_hook;

    // Real hardware boots executing BIOS from 0x00000000 in Supervisor
    // mode, IRQ/FIQ disabled (I and F bits set), ARM state (not Thumb).
    state->cpu.current_mode = GBA_MODE_SUPERVISOR;
    state->cpu.cpsr = (uint32_t)GBA_MODE_SUPERVISOR | CPSR_I_BIT | CPSR_F_BIT;
    state->cpu.thumb_mode = false;
    state->cpu.r[15] = 0x00000000;

    // None of gba_ppu.h/gba_apu.h/gba_dma.h/gba_timers.h declare a
    // separate _reset function -- only _init -- so re-running init is the
    // repeatable reset path for these, same as gba_cpu_reset does for CPU.
    gba_ppu_init(&state->ppu);
    gba_apu_init(&state->apu);
    gba_dma_init(&state->dma);
    gba_timers_init(&state->timers);
    ding_audio_reset(&state->audio_ring);

    return true;
}

// Hardware timing constants (per GBATEK), all in CPU cycles at 16.78 MHz.
static const uint32_t GBA_CYCLES_PER_HDRAW   = 960;
static const uint32_t GBA_CYCLES_PER_HBLANK  = 272;
static const uint32_t GBA_CYCLES_PER_SCANLINE = GBA_CYCLES_PER_HDRAW + GBA_CYCLES_PER_HBLANK; // 1232
static const int GBA_VISIBLE_LINES = 160;
static const int GBA_TOTAL_LINES   = 228; // 160 visible + 68 VBlank

// DISPSTAT bit positions (GBATEK) -- read/written directly on ppu.dispstat
// for now. TODO: not yet synced with memory.io's mapped register, so a
// game reading REG_DISPSTAT via gba_mem_read16 won't see these bits --
// needs wiring once gba_memory.cpp's I/O read/write side exists.
static const uint16_t DISPSTAT_VBLANK_FLAG    = 1 << 0;
static const uint16_t DISPSTAT_HBLANK_FLAG    = 1 << 1;
static const uint16_t DISPSTAT_VCOUNTER_FLAG  = 1 << 2;
static const uint16_t DISPSTAT_VBLANK_IRQ_EN  = 1 << 3;
static const uint16_t DISPSTAT_HBLANK_IRQ_EN  = 1 << 4;
static const uint16_t DISPSTAT_VCOUNT_IRQ_EN  = 1 << 5;

static uint16_t dispstat_vcount_setting(uint16_t dispstat) {
    return (dispstat >> 8) & 0xFF;
}

// Fires every DMA channel armed for `reason` (VBlank/HBlank/Special).
// gba_dma_trigger only arms; gba_dma_step performs the actual transfer.
// Both are documented as no-ops for channels that aren't armed/enabled,
// so it's safe to call for all 4 unconditionally.
static void fire_dma(GbaCoreState* state, GbaDmaTiming reason) {
    for (int ch = 0; ch < 4; ch++) {
        gba_dma_trigger(&state->dma, ch, reason);
        gba_dma_step(&state->dma, &state->memory, ch);
    }
}

// ADDED: closes the gap flagged in gba_apu.cpp's file-top note --
// gba_apu_timer_tick pops FIFO samples but has no GbaDmaState* to signal
// a refill with. Convention (per gba_dma.h/gba_apu.h comments): DMA1
// feeds FIFO A, DMA2 feeds FIFO B. Standard GBA behavior refills once the
// FIFO drops to half-full (16 of 32 bytes) rather than waiting for empty.
static void check_fifo_refill(GbaCoreState* state, int fifo_index, int dma_channel) {
    GbaDirectSoundFifo* fifo = (fifo_index == 0) ? &state->apu.fifo_a : &state->apu.fifo_b;
    if (fifo->count <= 16) {
        gba_dma_trigger(&state->dma, dma_channel, GBA_DMA_TIMING_SPECIAL);
        gba_dma_step(&state->dma, &state->memory, dma_channel);
    }
}

// Advances the whole machine by one frame: CPU runs instruction-by-
// instruction (variable cycle cost per gba_cpu_step's return value),
// PPU/timers/DMA/APU are advanced by that same cycle count, scanline by
// scanline, until VBlank is reached and a frame has been rendered.
void gba_core_run_frame(GbaCoreState* state) {
    for (int line = 0; line < GBA_TOTAL_LINES; line++) {
        state->ppu.vcount = (uint16_t)line;

        bool vcount_match = dispstat_vcount_setting(state->ppu.dispstat) == (uint16_t)line;
        if (vcount_match) {
            state->ppu.dispstat |= DISPSTAT_VCOUNTER_FLAG;
            if (state->ppu.dispstat & DISPSTAT_VCOUNT_IRQ_EN) {
                gba_interrupts_request(&state->interrupts, GBA_IRQ_VCOUNT);
            }
        } else {
            state->ppu.dispstat &= ~DISPSTAT_VCOUNTER_FLAG;
        }

        bool in_vblank = line >= GBA_VISIBLE_LINES;
        if (in_vblank) {
            state->ppu.dispstat |= DISPSTAT_VBLANK_FLAG;
        } else {
            state->ppu.dispstat &= ~DISPSTAT_VBLANK_FLAG;
        }

        // First line of VBlank: fire VBlank IRQ/DMA once, not every line.
        if (line == GBA_VISIBLE_LINES) {
            if (state->ppu.dispstat & DISPSTAT_VBLANK_IRQ_EN) {
                gba_interrupts_request(&state->interrupts, GBA_IRQ_VBLANK);
            }
            fire_dma(state, GBA_DMA_TIMING_VBLANK);
        }

        // --- HDraw portion of the line ---
        state->ppu.dispstat &= ~DISPSTAT_HBLANK_FLAG;
uint32_t hdraw_elapsed = 0;
        while (hdraw_elapsed < GBA_CYCLES_PER_HDRAW) {
            uint32_t cycles = gba_cpu_step(&state->cpu, &state->memory, &state->interrupts);
            gba_timers_step(&state->timers, cycles, &state->interrupts, &state->apu);
            gba_apu_step(&state->apu, cycles);
            check_fifo_refill(state, 0, 1); // FIFO A <- DMA1
            check_fifo_refill(state, 1, 2); // FIFO B <- DMA2
            hdraw_elapsed += cycles;
        }

        // --- HBlank portion of the line (skipped during VBlank lines --
        // HBlank IRQ/DMA still fire per GBATEK even while in VBlank, so
        // this intentionally does NOT check in_vblank). ---
        state->ppu.dispstat |= DISPSTAT_HBLANK_FLAG;
        if (state->ppu.dispstat & DISPSTAT_HBLANK_IRQ_EN) {
            gba_interrupts_request(&state->interrupts, GBA_IRQ_HBLANK);
        }
        fire_dma(state, GBA_DMA_TIMING_HBLANK);

uint32_t hblank_elapsed = 0;
        while (hblank_elapsed < GBA_CYCLES_PER_HBLANK) {
            uint32_t cycles = gba_cpu_step(&state->cpu, &state->memory, &state->interrupts);
            gba_timers_step(&state->timers, cycles, &state->interrupts, &state->apu);
            gba_apu_step(&state->apu, cycles);
            check_fifo_refill(state, 0, 1); // FIFO A <- DMA1
            check_fifo_refill(state, 1, 2); // FIFO B <- DMA2
            hblank_elapsed += cycles;
        }
    }

    // Per-frame render (see gba_ppu.h's top-of-file plan comment: whole
    // screen drawn in one pass from end-of-frame register/VRAM state,
    // not per-scanline -- HBlank-trick games will need the upgrade noted
    // there later).
    gba_ppu_render_frame(&state->ppu, &state->memory);

    // gba_apu_mix outputs int16 (interleaved stereo); ding_audio.h's ring
    // buffer wants float in [-1.0, 1.0], so convert through a scratch
    // buffer before pushing. GBA_AUDIO_SAMPLES_PER_FRAME is a fixed
    // approximation, not cycle-exact -- see that constant's definition.
    int16_t mix_scratch[GBA_AUDIO_SAMPLES_PER_FRAME * 2];
    gba_apu_mix(&state->apu, mix_scratch, GBA_AUDIO_SAMPLES_PER_FRAME);

    float float_scratch[GBA_AUDIO_SAMPLES_PER_FRAME * 2];
    for (uint32_t i = 0; i < GBA_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
        float_scratch[i] = mix_scratch[i] / 32768.0f;
    }
    ding_audio_write(&state->audio_ring, float_scratch, GBA_AUDIO_SAMPLES_PER_FRAME);
}

const uint32_t* gba_core_get_framebuffer(GbaCoreState* state) {
    return gba_ppu_get_framebuffer(&state->ppu);
}

uint32_t gba_core_get_audio_buffer(GbaCoreState* state, float* out_buf, uint32_t max_frames) {
    return ding_audio_read(&state->audio_ring, out_buf, max_frames);
}

// GBA-visible memory regions exposed for Cockpit/engine achievement
// reads. BIOS and cart save data are deliberately left out for now:
// BIOS reads while not executing from BIOS are supposed to return
// open-bus (last-fetched opcode), which doesn't fit DING_MEM_DIRECT's
// "raw pointer" contract -- and save_data isn't allocated yet (see
// gba_core_load_rom's TODO). Revisit both once those pieces land.
uint32_t gba_core_get_memory_region_count() {
    return 5; // EWRAM, IWRAM, Palette, VRAM, OAM
}

bool gba_core_get_memory_region(GbaCoreState* state, uint32_t index, DingMemoryRegion* out) {
    switch (index) {
        case 0:
            out->name = "EWRAM";
            out->base_addr = 0x02000000;
            out->size = sizeof(state->memory.ewram);
            out->ptr = state->memory.ewram;
            out->writable = 1;
            break;
        case 1:
            out->name = "IWRAM";
            out->base_addr = 0x03000000;
            out->size = sizeof(state->memory.iwram);
            out->ptr = state->memory.iwram;
            out->writable = 1;
            break;
        case 2:
            out->name = "Palette RAM";
            out->base_addr = 0x05000000;
            out->size = sizeof(state->memory.palette);
            out->ptr = state->memory.palette;
            out->writable = 1;
            break;
        case 3:
            out->name = "VRAM";
            out->base_addr = 0x06000000;
            out->size = sizeof(state->memory.vram);
            out->ptr = state->memory.vram;
            out->writable = 1;
            break;
        case 4:
            out->name = "OAM";
            out->base_addr = 0x07000000;
            out->size = sizeof(state->memory.oam);
            out->ptr = state->memory.oam;
            out->writable = 1;
            break;
        default:
            return false;
    }
    out->access = DING_MEM_DIRECT;
    out->read8 = nullptr;
    out->write8 = nullptr;
    return true;
}

// Serializes to the shared .ding format (ding_savestate.h). One block per
// subsystem, raw struct dump -- fine for now since every field in these
// structs is plain data; the moment any subsystem gains a pointer field
// that needs special handling (none do yet) this needs a real per-field
// writer instead of memcpy-the-whole-struct.
size_t gba_core_save_state(GbaCoreState* state, uint8_t* out, size_t out_size) {
    DingSaveWriter writer;
    if (ding_save_writer_init(&writer, out, out_size, "Game Boy Advance") != DING_SS_OK) {
        return 0;
    }

    ding_save_write_block(&writer, "CPU", &state->cpu, sizeof(state->cpu));
    // Memory block deliberately excludes rom/save_data (caller-owned
    // pointers, not core state) and bios_open_bus is small enough to not
    // bother splitting out -- whole struct minus the pointer fields would
    // need a hand-rolled layout; for now this writes the whole struct
    // including those pointers as garbage-on-reload placeholders. TODO:
    // split GbaMemory serialization to skip rom/save_data pointers once
    // this needs to actually round-trip correctly.
    ding_save_write_block(&writer, "MEM", &state->memory, sizeof(state->memory));
    ding_save_write_block(&writer, "PPU", &state->ppu, sizeof(state->ppu));
    ding_save_write_block(&writer, "APU", &state->apu, sizeof(state->apu));
    ding_save_write_block(&writer, "DMA", &state->dma, sizeof(state->dma));
    ding_save_write_block(&writer, "TIMERS", &state->timers, sizeof(state->timers));
    ding_save_write_block(&writer, "INTERRUPTS", &state->interrupts, sizeof(state->interrupts));

    size_t total_size = 0;
    if (ding_save_writer_finish(&writer, &total_size) != DING_SS_OK) {
        return 0;
    }
    return total_size;
}

DingResult gba_core_load_state(GbaCoreState* state, const uint8_t* data, size_t size) {
    DingSaveReader reader;
    DingSaveResult result = ding_save_reader_init(&reader, data, size);
    if (result != DING_SS_OK) {
        return DING_ERR_BAD_STATE;
    }

    // Preserve the caller-owned pointers in GbaMemory before the MEM
    // block overwrites the whole struct (see save-side TODO above).
    const uint8_t* rom_ptr = state->memory.rom;
    uint32_t rom_size = state->memory.rom_size;
    uint8_t* save_data_ptr = state->memory.save_data;
    uint32_t save_size = state->memory.save_size;

    ding_save_read_block(&reader, "CPU", &state->cpu, sizeof(state->cpu), nullptr);
    ding_save_read_block(&reader, "MEM", &state->memory, sizeof(state->memory), nullptr);
    ding_save_read_block(&reader, "PPU", &state->ppu, sizeof(state->ppu), nullptr);
    ding_save_read_block(&reader, "APU", &state->apu, sizeof(state->apu), nullptr);
    ding_save_read_block(&reader, "DMA", &state->dma, sizeof(state->dma), nullptr);
    ding_save_read_block(&reader, "TIMERS", &state->timers, sizeof(state->timers), nullptr);
    ding_save_read_block(&reader, "INTERRUPTS", &state->interrupts, sizeof(state->interrupts), nullptr);

    state->memory.rom = rom_ptr;
    state->memory.rom_size = rom_size;
    state->memory.save_data = save_data_ptr;
    state->memory.save_size = save_size;

    return DING_OK;
}

// No dynamic allocation currently happens anywhere in this core (save_data
// is still nullptr per gba_core_load_rom's TODO), so there's nothing to
// free yet. This just marks state as unusable until re-init, and is the
// hook point once save_data allocation lands.
void gba_core_shutdown(GbaCoreState* state) {
    state->bios.is_valid = false;
    state->memory.rom = nullptr;
    state->memory.rom_size = 0;
    // TODO: free state->memory.save_data here once gba_core_load_rom
    // actually allocates it.
}
