#include "gba_memory.h"

// Memory read/write dispatch across the GBA address space, plus save-type
// detection.
//
// Plan:
//  - gba_mem_read8/16/32 and write8/16/32: mask incoming address into
//    region (BIOS/EWRAM/IWRAM/IO/Palette/VRAM/OAM/ROM/SRAM) and route to
//    the right backing buffer. Watch alignment -- unaligned 16/32-bit
//    access on ARM has defined-but-weird rotate behavior we'll need to
//    match later.
//  - BIOS reads outside of BIOS execution return last-fetched-opcode
//    (open bus behavior) -- not real memory, per GBATEK. Note for later.
//  - gba_mem_detect_save_type: scan ROM for ID strings ("SRAM_V",
//    "EEPROM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V") per GBATEK
//    convention, since save type isn't in the ROM header.

#include <cstring>

void gba_mem_init(GbaMemory* mem, const uint8_t* bios, const uint8_t* rom, uint32_t rom_size) {
    std::memset(mem->bios, 0, sizeof(mem->bios));
    if (bios) {
        std::memcpy(mem->bios, bios, sizeof(mem->bios));
    }

    std::memset(mem->ewram, 0, sizeof(mem->ewram));
    std::memset(mem->iwram, 0, sizeof(mem->iwram));
    std::memset(mem->io, 0, sizeof(mem->io));
    std::memset(mem->palette, 0, sizeof(mem->palette));
    std::memset(mem->vram, 0, sizeof(mem->vram));
    std::memset(mem->oam, 0, sizeof(mem->oam));

    mem->rom = rom;
    mem->rom_size = rom_size;

    mem->save_data = nullptr;
    mem->save_size = 0;
    mem->save_type = GBA_SAVE_NONE;

    mem->bios_open_bus = 0;
}

// Returns a pointer to the backing byte for addr, masked into its region,
// or nullptr if addr falls in unmapped/unhandled space (I/O and SRAM are
// handled separately since they're not simple flat buffers long-term).
static uint8_t* gba_mem_resolve(GbaMemory* mem, uint32_t addr, uint32_t* out_mask) {
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x00: // BIOS
            *out_mask = sizeof(mem->bios) - 1;
            return mem->bios;
        case 0x02: // EWRAM
            *out_mask = sizeof(mem->ewram) - 1;
            return mem->ewram;
        case 0x03: // IWRAM
            *out_mask = sizeof(mem->iwram) - 1;
            return mem->iwram;
        case 0x05: // Palette
            *out_mask = sizeof(mem->palette) - 1;
            return mem->palette;
        case 0x06: // VRAM
            *out_mask = sizeof(mem->vram) - 1;
            return mem->vram;
        case 0x07: // OAM
            *out_mask = sizeof(mem->oam) - 1;
            return mem->oam;
        default:
            return nullptr;
    }
}

uint8_t gba_mem_read8(GbaMemory* mem, uint32_t addr) {
    uint32_t region = addr >> 24;

    if (region == 0x04) {
        // I/O registers: flat buffer for now, real per-register side
        // effects (DISPSTAT, timers, etc.) come later once those
        // subsystems exist.
        uint32_t off = addr & (sizeof(mem->io) - 1);
        return mem->io[off];
    }

    if (region >= 0x08 && region <= 0x0D) {
        // Cart ROM, mirrored across wait-state regions 0x08-0x0D
        if (mem->rom && mem->rom_size > 0) {
            uint32_t off = addr & (mem->rom_size - 1);
            return mem->rom[off];
        }
        return 0xFF; // open bus / no cart
    }

    if (region == 0x0E || region == 0x0F) {
        // Cart save data -- real per-type behavior (SRAM flat vs Flash
        // bank-switched vs EEPROM serial) comes with gba_mem_detect_save_type.
        if (mem->save_data && mem->save_size > 0) {
            uint32_t off = addr & (mem->save_size - 1);
            return mem->save_data[off];
        }
        return 0xFF;
    }

    uint32_t mask = 0;
    uint8_t* buf = gba_mem_resolve(mem, addr, &mask);
    if (buf) {
        return buf[addr & mask];
    }

    // Unmapped region: return low byte of BIOS open-bus value as a stand-in
    // until real per-region open-bus behavior is modeled.
    return (uint8_t)(mem->bios_open_bus & 0xFF);
}

uint16_t gba_mem_read16(GbaMemory* mem, uint32_t addr) {
    addr &= ~1u; // force halfword alignment (real HW rotates on misalign, TODO later)
    uint8_t lo = gba_mem_read8(mem, addr);
    uint8_t hi = gba_mem_read8(mem, addr + 1);
    return (uint16_t)(lo | (hi << 8));
}

uint32_t gba_mem_read32(GbaMemory* mem, uint32_t addr) {
    addr &= ~3u; // force word alignment (real HW rotates on misalign, TODO later)
    uint16_t lo = gba_mem_read16(mem, addr);
    uint16_t hi = gba_mem_read16(mem, addr + 2);
    return (uint32_t)lo | ((uint32_t)hi << 16);
}

void gba_mem_write8(GbaMemory* mem, uint32_t addr, uint8_t val) {
    uint32_t region = addr >> 24;

    if (region == 0x00) {
        return; // BIOS is read-only
    }

    if (region == 0x04) {
        uint32_t off = addr & (sizeof(mem->io) - 1);
        mem->io[off] = val;
        return;
    }

    if (region >= 0x08 && region <= 0x0D) {
        return; // Cart ROM is read-only
    }

    if (region == 0x0E || region == 0x0F) {
        if (mem->save_data && mem->save_size > 0) {
            uint32_t off = addr & (mem->save_size - 1);
            mem->save_data[off] = val;
        }
        return;
    }

    uint32_t mask = 0;
    uint8_t* buf = gba_mem_resolve(mem, addr, &mask);
    if (buf) {
        buf[addr & mask] = val;
    }
    // Unmapped region: write is silently dropped.
}

void gba_mem_write16(GbaMemory* mem, uint32_t addr, uint16_t val) {
    addr &= ~1u; // force halfword alignment (TODO: real misalign behavior)
    gba_mem_write8(mem, addr, (uint8_t)(val & 0xFF));
    gba_mem_write8(mem, addr + 1, (uint8_t)((val >> 8) & 0xFF));
}

void gba_mem_write32(GbaMemory* mem, uint32_t addr, uint32_t val) {
    addr &= ~3u; // force word alignment (TODO: real misalign behavior)
    gba_mem_write16(mem, addr, (uint16_t)(val & 0xFFFF));
    gba_mem_write16(mem, addr + 2, (uint16_t)((val >> 16) & 0xFFFF));
}

GbaSaveType gba_mem_detect_save_type(const uint8_t* rom, uint32_t rom_size) {
    if (!rom || rom_size == 0) {
        return GBA_SAVE_NONE;
    }

    // GBATEK convention: save type isn't in the header, so scan the ROM
    // for one of these ID strings, which real cart linker scripts embed.
    // Check longer/more specific strings first to avoid a short match
    // (e.g. "FLASH_V") shadowing a more specific one (e.g. "FLASH512_V").
    struct SaveMarker {
        const char* id;
        GbaSaveType type;
    };
    static const SaveMarker markers[] = {
        { "FLASH512_V", GBA_SAVE_FLASH512 },
        { "FLASH1M_V",  GBA_SAVE_FLASH1M },
        { "FLASH_V",    GBA_SAVE_FLASH512 }, // plain FLASH_V is 512kb per GBATEK
        { "SRAM_V",     GBA_SAVE_SRAM },
        { "EEPROM_V",   GBA_SAVE_EEPROM },
    };

    for (const auto& marker : markers) {
        size_t id_len = std::strlen(marker.id);
        if (rom_size < id_len) continue;

        for (uint32_t i = 0; i <= rom_size - id_len; i++) {
            if (std::memcmp(rom + i, marker.id, id_len) == 0) {
                return marker.type;
            }
        }
    }

    return GBA_SAVE_NONE;
}
