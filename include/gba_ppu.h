#ifndef GBA_PPU_H
#define GBA_PPU_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// GBA PPU: 6 background modes
//  Mode 0-1: tile/tilemap, up to 4 BG layers, modes 1-2 add affine transform
//  Mode 2:   tile/tilemap, affine only (2 layers)
//  Mode 3-5: bitmap framebuffer modes (simpler, less common)
// Sprites (OBJs): up to 128 from OAM, support affine transform, priority
// vs backgrounds.
//
// RENDER STRATEGY: per-frame first (snapshot registers, draw whole screen
// in one pass) to get ROMs on screen faster, matching interpreter-over-JIT
// philosophy on the CPU side.
//
// REMINDER FOR LATER: per-frame will break games using HBlank IRQ tricks
// (mid-frame scroll/palette changes -- wavy water, split-screen effects,
// etc). Upgrade to per-scanline rendering once basics work and we hit
// games that need it.

// TODO: struct GbaPpuState
//   - registers: DISPCNT, DISPSTAT, BG0-3CNT, scroll/affine regs, etc.
//   - framebuffer (240x160, format TBD -- likely RGBA8888 for output)
//   - reference to GbaMemory for VRAM/OAM/Palette access

// TODO: gba_ppu_init(...)
// TODO: gba_ppu_render_frame(GbaPpuState* ppu, GbaMemory* mem)
// TODO: gba_ppu_get_framebuffer(...)

#ifdef __cplusplus
}
#endif

#endif // GBA_PPU_H
