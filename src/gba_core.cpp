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
