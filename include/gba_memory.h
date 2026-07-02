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

// TODO: struct GbaMemory
//   - uint8_t* ewram / iwram / vram / oam / palette
//   - uint8_t* rom, size_t rom_size
//   - save data buffer + detected save type (SRAM/Flash/EEPROM/None)
//   - DingMemoryRegion mappings for Cockpit/engine access

// TODO: gba_mem_init(...)
// TODO: gba_mem_read8/16/32(...)
// TODO: gba_mem_write8/16/32(...)
// TODO: gba_mem_detect_save_type(...)  string-scan ROM per GBATEK convention

#ifdef __cplusplus
}
#endif

#endif // GBA_MEMORY_H
