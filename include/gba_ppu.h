#ifndef GBA_PPU_H
#define GBA_PPU_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

struct GbaMemory; // fwd decl, defined in gba_memory.h

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

#define GBA_SCREEN_WIDTH  240
#define GBA_SCREEN_HEIGHT 160

typedef struct {
    int16_t ref_x, ref_y;   // affine reference point (BG2X/Y, BG3X/Y), fixed-point 20.8
    int16_t pa, pb, pc, pd; // affine transform matrix (BG2PA-PD, BG3PA-PD), fixed-point 8.8
} GbaAffineParams;

typedef struct {
    uint16_t control;   // BGxCNT
    uint16_t scroll_x;  // BGxHOFS
    uint16_t scroll_y;  // BGxVOFS
} GbaBgLayer;

typedef struct {
    // Per-frame rendering (see file's top-of-file plan comment): registers
    // are read at render time rather than tracked mid-scanline, so this
    // struct only needs to hold current values, not per-line snapshots.
    uint16_t dispcnt;
    uint16_t dispstat;
    uint16_t vcount;

    GbaBgLayer bg[4];           // BG0-3
    GbaAffineParams affine[2];  // BG2, BG3 (only these two support affine)

    uint32_t framebuffer[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT]; // RGBA8888
} GbaPpuState;

void gba_ppu_init(GbaPpuState* ppu);

// Renders one full frame in one pass (see file's top-of-file plan comment
// re: per-frame vs per-scanline). Reads current register/VRAM/OAM state
// from `mem` and writes into ppu->framebuffer.
void gba_ppu_render_frame(GbaPpuState* ppu, struct GbaMemory* mem);

const uint32_t* gba_ppu_get_framebuffer(const GbaPpuState* ppu);

#ifdef __cplusplus
}
#endif

#endif // GBA_PPU_H
