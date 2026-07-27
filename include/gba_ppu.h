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
    // FIXED: was int16_t, too narrow -- BG2X/Y, BG3X/Y are 28-bit signed
    // 20.8 fixed-point values (sign-extended into these 32-bit fields by
    // the IO write handler).
    int32_t ref_x, ref_y;   // affine reference point (BG2X/Y, BG3X/Y), fixed-point 20.8
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
    // ADDED: window registers (WIN0H/V, WIN1H/V, WININ, WINOUT). Enable
    // bits for Win0/Win1/ObjWin live in dispcnt (bits 13-15), not
    // duplicated here.
    uint16_t win0h, win0v; // bits 15-8=X1/Y1 (left/top), bits 7-0=X2/Y2 (right/bottom, exclusive)
    uint16_t win1h, win1v;
    uint16_t winin;  // bits 0-5 = Win0 BG0-3/OBJ/effect enable, bits 8-13 = Win1 same
    uint16_t winout; // bits 0-5 = Outside BG0-3/OBJ/effect enable, bits 8-13 = ObjWin same (ObjWin containment itself deferred, see gba_ppu.cpp)
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
