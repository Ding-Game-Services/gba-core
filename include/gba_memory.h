#ifndef GBA_MEMORY_H
#define GBA_MEMORY_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// GBA memory map regions (addresses per GBATEK)
// 0x00000000 - BIOS      (16 KB,  not bundled — DingBiosDescriptor)
// 0x02000000 - EWRAM     (256 KB, slow external work RAM)
// 0x03000000 - IWRAM     (32 KB,  fast internal work RAM)
// 0x04000000 - I/O Registers
// 0x05000000 - Palette RAM (1 KB)
// 0x06000000 - VRAM      (96 KB)
// 0x07000000 - OAM       (1 KB, sprite attributes)
// 0x08000000 - Cart ROM  (up to 32 MB, mirrored across 3 wait-state regions)
// 0x0E000000 - Cart SRAM/Flash/EEPROM (save data, type detected via ROM scan)

typedef enum {
    GBA_SAVE_NONE = 0,
    GBA_SAVE_SRAM,
    GBA_SAVE_EEPROM,
    GBA_SAVE_FLASH512,
    GBA_SAVE_FLASH1M
} GbaSaveType;

typedef struct GbaMemory {
    uint8_t bios[0x4000];      // 16 KB, caller-supplied real dump
    uint8_t ewram[0x40000];    // 256 KB
    uint8_t iwram[0x8000];     // 32 KB
    uint8_t io[0x400];         // 1 KB, register-mapped
    uint8_t palette[0x400];    // 1 KB
    uint8_t vram[0x18000];     // 96 KB
    uint8_t oam[0x400];        // 1 KB

    const uint8_t* rom;        // caller-owned, not copied
    uint32_t rom_size;

    uint8_t* save_data;        // allocated per detected save type
    uint32_t save_size;
    GbaSaveType save_type;

    uint32_t bios_open_bus;    // last-fetched opcode, for BIOS open-bus reads

    // ADDED: IO-write dispatch hook. gba_mem_write8/16/32 only stores
    // bytes into the flat `io` buffer -- something needs to actually call
    // gba_dma_write_control/gba_timers_write_control/etc when a game
    // writes those registers, and gba_memory.cpp intentionally doesn't
    // include dma/timers/ppu/apu headers to stay decoupled. Kept generic
    // (void* context + function pointer) so the core (gba_core.cpp) can
    // wire up the real dispatch without this file depending on it.
    // Fires from gba_mem_write16 only (see gba_memory.cpp note) -- GBA IO
    // registers are conventionally halfword-addressable; gba_mem_write32
    // already decomposes into two write16 calls so 32-bit register writes
    // (DMA SAD/DAD, FIFO_A/B) still reach the hook, just as two halves.
    void* io_hook_context;
    void (*io_write_hook)(void* context, uint32_t addr, uint16_t value);
} GbaMemory;

void gba_mem_init(GbaMemory* mem, const uint8_t* bios, const uint8_t* rom, uint32_t rom_size);
uint8_t  gba_mem_read8(GbaMemory* mem, uint32_t addr);
uint16_t gba_mem_read16(GbaMemory* mem, uint32_t addr);
uint32_t gba_mem_read32(GbaMemory* mem, uint32_t addr);
void gba_mem_write8(GbaMemory* mem, uint32_t addr, uint8_t val);
void gba_mem_write16(GbaMemory* mem, uint32_t addr, uint16_t val);
void gba_mem_write32(GbaMemory* mem, uint32_t addr, uint32_t val);
GbaSaveType gba_mem_detect_save_type(const uint8_t* rom, uint32_t rom_size);

#ifdef __cplusplus
}
#endif

#endif // GBA_MEMORY_H
