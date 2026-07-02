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

// TODO: void gba_mem_init(GbaMemory* mem, const uint8_t* rom, size_t rom_size)
// TODO: uint8_t  gba_mem_read8(GbaMemory* mem, uint32_t addr)
// TODO: uint16_t gba_mem_read16(GbaMemory* mem, uint32_t addr)
// TODO: uint32_t gba_mem_read32(GbaMemory* mem, uint32_t addr)
// TODO: void gba_mem_write8(GbaMemory* mem, uint32_t addr, uint8_t val)
// TODO: void gba_mem_write16(GbaMemory* mem, uint32_t addr, uint16_t val)
// TODO: void gba_mem_write32(GbaMemory* mem, uint32_t addr, uint32_t val)
// TODO: GbaSaveType gba_mem_detect_save_type(const uint8_t* rom, size_t rom_size)
