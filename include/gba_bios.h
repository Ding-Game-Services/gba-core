#ifndef GBA_BIOS_H
#define GBA_BIOS_H

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// BIOS handling for GBA core.
//
// CURRENT PLAN: require a real BIOS dump (user-supplied, per
// DingBiosDescriptor -- never bundled). CPU executes real BIOS code at
// reset and on SWI calls, same as hardware. This gets us accurate
// behavior without having to hand-implement each BIOS function.
//
// FUTURE (low priority, after core basics work): HLE (High-Level
// Emulation) fallback -- native C++ implementations of common SWI calls
// (Div, Sqrt, CpuSet, VBlankIntrWait, etc.) for when no BIOS dump is
// available. Deferred until real-BIOS path is solid.
//
// OPEN BUS NOTE: reads from BIOS address space while PC is NOT executing
// from BIOS should return the last opcode fetched from BIOS, not real
// BIOS bytes (see gba_memory.cpp note). Only matters once BIOS execution
// itself works.

// TODO: struct GbaBiosDescriptor
//   - const uint8_t* data
//   - size_t size          (should be exactly 16 KB for real GBA BIOS)
//   - bool is_valid         (checksum/size validation result)

// TODO: bool gba_bios_load(GbaBiosDescriptor* bios, const uint8_t* data, size_t size)
// TODO: bool gba_bios_validate(const GbaBiosDescriptor* bios)

#ifdef __cplusplus
}
#endif

#endif // GBA_BIOS_H
