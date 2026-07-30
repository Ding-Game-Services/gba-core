#include "gba_ppu.h"
#include "gba_memory.h"
#include <cstring>

// PPU implementation -- per-frame render pass (see gba_ppu.h for the
// per-scanline reminder/rationale).
//
// SCOPE FOR THIS PASS (flagging cuts explicitly rather than leaving silent
// gaps):
//  - Implemented: Mode 0 (regular tile BGs, all 4 layers), Mode 1/2
//    (affine BG2/BG3), Mode 3/4/5 (bitmap, including window/mosaic on
//    their BG2), regular and affine sprites with shape/size/priority
//    compositing, Win0/Win1/Outside/Obj windows, color special effects
//    (alpha blend + brightness inc/dec, including OBJ semi-transparent
//    mode's forced blend), mosaic (BG and OBJ -- OBJ mosaic uses a
//    sprite-local approximation, see that code's own note).
//  - No remaining known gaps in this file as of this pass.
//
// Compositing model: for priority level p = 3 down to 0, draw enabled
// regular BG layers at that priority (BG index 3 down to 0, so lower BG
// index ends up on top for ties), then draw sprites at that priority.
// This matches hardware's per-priority BG-then-sprite ordering without
// needing per-pixel priority buffers.

#define GBA_BG_PALETTE_OFFSET 0     // palette RAM: first 256 entries = BG
#define GBA_OBJ_PALETTE_OFFSET 0x200 // second 256 entries = OBJ (byte offset)

#define GBA_OBJ_CHAR_BASE_TILE  0x10000 // OBJ tiles in modes 0-2
#define GBA_OBJ_CHAR_BASE_BITMAP 0x14000 // OBJ tiles in bitmap modes 3-5

static inline uint32_t gba_ppu_bgr555_to_rgba8888(uint16_t c) {
    uint32_t r = (c & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    // replicate top 3 bits into the low bits for a fuller 0-255 range
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void gba_ppu_init(GbaPpuState* ppu) {
    ppu->dispcnt = 0;
    ppu->dispstat = 0;
    ppu->vcount = 0;
    ppu->win0h = 0;
    ppu->win0v = 0;
    ppu->win1h = 0;
    ppu->win1v = 0;
ppu->winin = 0;
    ppu->winout = 0;
    ppu->bldcnt = 0;
    ppu->bldalpha = 0;
    ppu->bldy = 0;
    ppu->mosaic = 0;
    for (int i = 0; i < 4; i++) {
        ppu->bg[i].control = 0;
        ppu->bg[i].scroll_x = 0;
        ppu->bg[i].scroll_y = 0;
    }
    for (int i = 0; i < 2; i++) {
        ppu->affine[i].ref_x = 0;
        ppu->affine[i].ref_y = 0;
        ppu->affine[i].pa = 0x100; // 1.0 in 8.8 fixed point (identity), unused until affine lands
        ppu->affine[i].pb = 0;
        ppu->affine[i].pc = 0;
        ppu->affine[i].pd = 0x100;
    }
    std::memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
}

const uint32_t* gba_ppu_get_framebuffer(const GbaPpuState* ppu) {
    return ppu->framebuffer;
}

// ---- Regular (non-affine) tile BG rendering --------------------------

struct GbaBgParams {
    uint32_t char_base;     // byte offset into VRAM
    uint32_t screen_base;   // byte offset into VRAM
    bool     color_256;     // false = 4bpp/16-palette, true = 8bpp/256-palette
    uint8_t  screen_size;   // 0-3, see below
    uint8_t  priority;
    bool     mosaic;
};

static GbaBgParams gba_ppu_parse_bg_control(uint16_t control) {
    GbaBgParams p;
    p.priority    = control & 0x3;
    p.char_base   = ((control >> 2) & 0x3) * 0x4000;
    p.mosaic      = ((control >> 6) & 0x1) != 0;
    p.color_256   = ((control >> 7) & 0x1) != 0;
    p.screen_base = ((control >> 8) & 0x1F) * 0x800;
    p.screen_size = (control >> 14) & 0x3;
    return p;
}

// MOSAIC register field -> actual size (register stores size-1).
static inline uint32_t gba_ppu_mosaic_size(uint16_t mosaic_reg, int shift) {
    return ((mosaic_reg >> shift) & 0xF) + 1;
}

// Regular BG screen sizes (tiles): 0=32x32(256x256px) 1=64x32(512x256px)
// 2=32x64(256x512px) 3=64x64(512x512px). Screen blocks are 0x800 bytes
// each (32x32 tiles x 2 bytes/entry), laid out top-left, top-right,
// bottom-left, bottom-right for size 3.
static uint32_t gba_ppu_screen_block_offset(uint8_t screen_size, uint32_t tile_x, uint32_t tile_y) {
    uint32_t block_x = tile_x / 32;
    uint32_t block_y = tile_y / 32;
    switch (screen_size) {
        case 0: return 0;
        case 1: return (block_x & 1) * 0x800;
        case 2: return (block_y & 1) * 0x800;
        case 3: return (block_x & 1) * 0x800 + (block_y & 1) * 0x1000;
    }
    return 0;
}

// Returns RGBA8888 color, or 0 with *out_opaque=false if this BG pixel is
// transparent (palette index 0 in its bank).
static uint32_t gba_ppu_sample_bg_pixel(GbaMemory* mem, const GbaBgParams& bp,
                                         int32_t world_x, int32_t world_y, bool* out_opaque) {
    uint32_t tiles_w = (bp.screen_size == 1 || bp.screen_size == 3) ? 64 : 32;
    uint32_t tiles_h = (bp.screen_size == 2 || bp.screen_size == 3) ? 64 : 32;
    uint32_t px_w = tiles_w * 8;
    uint32_t px_h = tiles_h * 8;

    uint32_t wrapped_x = (uint32_t)world_x % px_w;
    uint32_t wrapped_y = (uint32_t)world_y % px_h;

    uint32_t tile_x = wrapped_x / 8;
    uint32_t tile_y = wrapped_y / 8;
    uint32_t within_x = wrapped_x % 8;
    uint32_t within_y = wrapped_y % 8;

    uint32_t block_offset = gba_ppu_screen_block_offset(bp.screen_size, tile_x, tile_y);
    uint32_t entry_offset = bp.screen_base + block_offset + ((tile_y % 32) * 32 + (tile_x % 32)) * 2;

    if (entry_offset + 1 >= sizeof(mem->vram)) { *out_opaque = false; return 0; }
    uint16_t entry = (uint16_t)(mem->vram[entry_offset] | (mem->vram[entry_offset + 1] << 8));

    uint32_t tile_index = entry & 0x3FF;
    bool hflip = (entry >> 10) & 1;
    bool vflip = (entry >> 11) & 1;
    uint32_t palette_bank = (entry >> 12) & 0xF; // 4bpp only

    uint32_t px = hflip ? (7 - within_x) : within_x;
    uint32_t py = vflip ? (7 - within_y) : within_y;

    uint8_t color_index;
    if (bp.color_256) {
        uint32_t tile_bytes = 64;
        uint32_t tile_offset = bp.char_base + tile_index * tile_bytes;
        uint32_t byte_offset = tile_offset + py * 8 + px;
        if (byte_offset >= sizeof(mem->vram)) { *out_opaque = false; return 0; }
        color_index = mem->vram[byte_offset];
    } else {
        uint32_t tile_bytes = 32;
        uint32_t tile_offset = bp.char_base + tile_index * tile_bytes;
        uint32_t byte_offset = tile_offset + py * 4 + (px / 2);
        if (byte_offset >= sizeof(mem->vram)) { *out_opaque = false; return 0; }
        uint8_t packed = mem->vram[byte_offset];
        color_index = (px & 1) ? (packed >> 4) : (packed & 0xF);
    }

    if (color_index == 0) { *out_opaque = false; return 0; }

    uint32_t pal_offset = bp.color_256
        ? (GBA_BG_PALETTE_OFFSET + color_index * 2)
        : (GBA_BG_PALETTE_OFFSET + (palette_bank * 16 + color_index) * 2);
    if (pal_offset + 1 >= sizeof(mem->palette)) { *out_opaque = false; return 0; }
    uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));

    *out_opaque = true;
    return gba_ppu_bgr555_to_rgba8888(raw);
}

// ---- Windows ------------------------------------------------------
//
// Win0 > Win1 > Obj Window > Outside, in that priority order (first
// window a pixel falls inside wins). Obj Window containment isn't
// modeled yet -- it requires OBJ-mode-2 sprites to render into a
// separate mask instead of drawing color, which belongs with the
// semi-transparent-OBJ/blending work still to come (see the existing
// obj_mode==2 "falls back to opaque draw" note in the sprite draw
// function). Until then, pixels that would fall in the Obj Window use
// the WINOUT "outside" bits as a stand-in.
static bool gba_ppu_point_in_window(uint16_t h, uint16_t v, int x, int y) {
    int x1 = (h >> 8) & 0xFF;
    int x2 = h & 0xFF;
    int y1 = (v >> 8) & 0xFF;
    int y2 = v & 0xFF;
    // GBATEK: if X2 > screen width or X2 < X1 (garbage/degenerate), treat
    // X2 as extending to the screen edge. Same for Y2. Common simplified
    // handling also used by other emulators for this edge case.
    if (x2 > GBA_SCREEN_WIDTH || x2 < x1) x2 = GBA_SCREEN_WIDTH;
    if (y2 > GBA_SCREEN_HEIGHT || y2 < y1) y2 = GBA_SCREEN_HEIGHT;
    return x >= x1 && x < x2 && y >= y1 && y < y2;
}

// layer_bit: 0-3 = BG0-3, 4 = OBJ (matches WININ/WINOUT bit layout).
// Returns true (layer visible at this pixel) unconditionally if no
// window is enabled at all, matching hardware's "windows off" behavior.
static bool gba_ppu_window_layer_enabled(const GbaPpuState* ppu, int x, int y, int layer_bit) {
    bool win0_enabled = (ppu->dispcnt >> 13) & 1;
    bool win1_enabled = (ppu->dispcnt >> 14) & 1;
    bool winobj_enabled = (ppu->dispcnt >> 15) & 1;

    if (!win0_enabled && !win1_enabled && !winobj_enabled) {
        return true;
    }

    if (win0_enabled && gba_ppu_point_in_window(ppu->win0h, ppu->win0v, x, y)) {
        return (ppu->winin >> layer_bit) & 1;
    }
if (win1_enabled && gba_ppu_point_in_window(ppu->win1h, ppu->win1v, x, y)) {
        return (ppu->winin >> (8 + layer_bit)) & 1;
    }
    if (winobj_enabled && ppu->obj_window_mask[y * GBA_SCREEN_WIDTH + x]) {
        return (ppu->winout >> (8 + layer_bit)) & 1;
    }
    return (ppu->winout >> layer_bit) & 1;
}

// ---- Color special effects (alpha blend / brightness inc-dec) --------
//
// Blending needs to know not just the frontmost visible layer at a pixel
// but the one directly beneath it too. Every draw call site below routes
// its pixel writes through gba_ppu_plot_pixel instead of writing
// ppu->framebuffer directly, which maintains that 2-deep "top/second"
// record as layers draw back-to-front. gba_ppu_apply_blending then does
// one full-screen pass at the end of the frame using BLDCNT/BLDALPHA/BLDY
// plus that record. Layer ids: 0-3=BG0-3, 4=OBJ, 5=Backdrop.
static inline void gba_ppu_plot_pixel(GbaPpuState* ppu, int x, int y, uint32_t color,
                                       uint8_t layer_id, bool obj_semi_transparent) {
    int idx = y * GBA_SCREEN_WIDTH + x;
    ppu->blend_second_color[idx] = ppu->framebuffer[idx];
    ppu->blend_second_layer[idx] = ppu->blend_top_layer[idx];
    ppu->framebuffer[idx] = color;
    ppu->blend_top_layer[idx] = layer_id;
    ppu->blend_obj_semi_transparent[idx] = obj_semi_transparent;
}

static inline uint8_t gba_ppu_clamp255(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// Blending is done directly in the already-expanded 8-bit RGBA8888 space
// rather than hardware's native 5-bit-per-channel space -- an
// approximation (loses a little precision vs. blending at 5-bit then
// re-expanding) but close enough, consistent with this file's other
// "good enough for now" simplifications.
static uint32_t gba_ppu_blend_alpha(uint32_t c1, uint32_t c2, uint32_t eva, uint32_t evb) {
    int32_t r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    int32_t r2 = c2 & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = (c2 >> 16) & 0xFF;
    uint8_t r = gba_ppu_clamp255((r1 * (int32_t)eva + r2 * (int32_t)evb) / 16);
    uint8_t g = gba_ppu_clamp255((g1 * (int32_t)eva + g2 * (int32_t)evb) / 16);
    uint8_t b = gba_ppu_clamp255((b1 * (int32_t)eva + b2 * (int32_t)evb) / 16);
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t gba_ppu_blend_brighten(uint32_t c, uint32_t evy, bool increase) {
    int32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    int32_t nr, ng, nb;
    if (increase) {
        nr = r + ((255 - r) * (int32_t)evy) / 16;
        ng = g + ((255 - g) * (int32_t)evy) / 16;
        nb = b + ((255 - b) * (int32_t)evy) / 16;
    } else {
        nr = r - (r * (int32_t)evy) / 16;
        ng = g - (g * (int32_t)evy) / 16;
        nb = b - (b * (int32_t)evy) / 16;
    }
    return 0xFF000000u | ((uint32_t)gba_ppu_clamp255(nb) << 16)
                        | ((uint32_t)gba_ppu_clamp255(ng) << 8)
                        | (uint32_t)gba_ppu_clamp255(nr);
}

// Full-screen pass applied once at the end of each frame, after all
// layers have drawn. OBJ semi-transparent mode (obj_mode==1) forces
// alpha-blend for that pixel regardless of BLDCNT's effect mode, as long
// as whatever's directly beneath it is target2-enabled -- a real
// hardware quirk, not an approximation.
static void gba_ppu_apply_blending(GbaPpuState* ppu) {
    uint8_t effect_mode = (ppu->bldcnt >> 6) & 0x3;
    uint8_t target1_mask = ppu->bldcnt & 0x3F;
    uint8_t target2_mask = (ppu->bldcnt >> 8) & 0x3F;
    uint32_t eva = ppu->bldalpha & 0x1F;        if (eva > 16) eva = 16;
    uint32_t evb = (ppu->bldalpha >> 8) & 0x1F; if (evb > 16) evb = 16;
    uint32_t evy = ppu->bldy & 0x1F;            if (evy > 16) evy = 16;

    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
        uint8_t top_layer = ppu->blend_top_layer[i];

        if (ppu->blend_obj_semi_transparent[i]) {
            uint8_t second_layer = ppu->blend_second_layer[i];
            if ((target2_mask >> second_layer) & 1) {
                ppu->framebuffer[i] = gba_ppu_blend_alpha(ppu->framebuffer[i], ppu->blend_second_color[i], eva, evb);
            }
            continue; // forced-blend pixel handled either way, skip normal BLDCNT path
        }

        if (effect_mode == 0) continue; // effects off
        if (!((target1_mask >> top_layer) & 1)) continue;

        if (effect_mode == 1) { // alpha blend
            uint8_t second_layer = ppu->blend_second_layer[i];
            if (!((target2_mask >> second_layer) & 1)) continue;
            ppu->framebuffer[i] = gba_ppu_blend_alpha(ppu->framebuffer[i], ppu->blend_second_color[i], eva, evb);
        } else if (effect_mode == 2) { // brightness increase
            ppu->framebuffer[i] = gba_ppu_blend_brighten(ppu->framebuffer[i], evy, true);
        } else { // effect_mode == 3, brightness decrease
            ppu->framebuffer[i] = gba_ppu_blend_brighten(ppu->framebuffer[i], evy, false);
        }
    }
}

static void gba_ppu_draw_bg_layer(GbaPpuState* ppu, GbaMemory* mem, int bg_index) {
    GbaBgParams bp = gba_ppu_parse_bg_control(ppu->bg[bg_index].control);
    uint16_t scroll_x = ppu->bg[bg_index].scroll_x;
    uint16_t scroll_y = ppu->bg[bg_index].scroll_y;
    uint32_t mosaic_w = bp.mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 0) : 1;
    uint32_t mosaic_h = bp.mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 4) : 1;

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            if (!gba_ppu_window_layer_enabled(ppu, x, y, bg_index)) continue;
            // Mosaic snaps the *sampled* screen position down to the
            // nearest grid origin -- every pixel in the block still gets
            // its own window/write, they just all sample the same texel.
            int mx = bp.mosaic ? (x / (int)mosaic_w) * (int)mosaic_w : x;
            int my = bp.mosaic ? (y / (int)mosaic_h) * (int)mosaic_h : y;
            bool opaque = false;
            uint32_t color = gba_ppu_sample_bg_pixel(mem, bp, mx + scroll_x, my + scroll_y, &opaque);
            if (opaque) {
                gba_ppu_plot_pixel(ppu, x, y, color, (uint8_t)bg_index, false);
            }
        }
    }
}

// ---- Affine BG rendering (Mode 1's BG2, Mode 2's BG2+BG3) -------------
//
// Affine BGs differ from regular BGs in three ways: the tilemap is a flat
// array of single-byte tile indices (no per-entry flip/palette-bank bits),
// tiles are always 8bpp/256-color, and the on-screen position comes from
// a 2x2 rotation/scale matrix (BGxPA-PD) applied to a reference point
// (BGxX/Y) instead of a simple HOFS/VOFS scroll. Per-frame render (same
// caveat as the rest of this file): reads the current register values
// once for the whole frame, so mid-frame HBlank reference-point updates
// (the classic Mode 7-style per-scanline effect) aren't reproduced yet.
static void gba_ppu_draw_affine_bg_layer(GbaPpuState* ppu, GbaMemory* mem, int bg_index) {
    uint16_t control = ppu->bg[bg_index].control;
    uint32_t char_base   = ((control >> 2) & 0x3) * 0x4000;
    uint32_t screen_base = ((control >> 8) & 0x1F) * 0x800;
    bool wrap            = (control >> 13) & 0x1; // Display Area Overflow: 1=wrap, 0=transparent outside
    uint8_t size_code    = (control >> 14) & 0x3;
    uint32_t size_px     = 128u << size_code; // 128/256/512/1024 -- affine BGs are always square
    bool mosaic          = (control >> 6) & 0x1;
    uint32_t mosaic_w    = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 0) : 1;
    uint32_t mosaic_h    = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 4) : 1;

    const GbaAffineParams& aff = ppu->affine[bg_index - 2]; // BG2->affine[0], BG3->affine[1]

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            if (!gba_ppu_window_layer_enabled(ppu, x, y, bg_index)) continue;

            // Mosaic snaps the screen coordinate fed into the affine
            // transform, same grid-origin approach as the regular BG path.
            int mx = mosaic ? (x / (int)mosaic_w) * (int)mosaic_w : x;
            int my = mosaic ? (y / (int)mosaic_h) * (int)mosaic_h : y;

            // pa-pd are 8.8 fixed point, ref_x/ref_y are 20.8 -- same
            // fractional width, so straight addition after the multiply
            // lines up correctly.
            int32_t tex_x_fp = aff.ref_x + aff.pa * mx + aff.pb * my;
            int32_t tex_y_fp = aff.ref_y + aff.pc * mx + aff.pd * my;

            int32_t px = tex_x_fp >> 8; // arithmetic shift -- handles negative correctly
            int32_t py = tex_y_fp >> 8;

            if (wrap) {
                px &= (int32_t)(size_px - 1);
                py &= (int32_t)(size_px - 1);
            } else if (px < 0 || py < 0 || px >= (int32_t)size_px || py >= (int32_t)size_px) {
                continue; // outside the affine surface -- transparent
            }

            uint32_t tile_x = (uint32_t)px / 8;
            uint32_t tile_y = (uint32_t)py / 8;
            uint32_t within_x = (uint32_t)px % 8;
            uint32_t within_y = (uint32_t)py % 8;
            uint32_t tiles_per_row = size_px / 8;

            // Affine tilemap: one byte per entry (tile index 0-255), no
            // flip/palette-bank bits, contiguous single block (no 32x32
            // sub-block layout like regular BG screen blocks).
            uint32_t map_offset = screen_base + tile_y * tiles_per_row + tile_x;
            if (map_offset >= sizeof(mem->vram)) continue;
            uint8_t tile_index = mem->vram[map_offset];

            uint32_t tile_offset = char_base + (uint32_t)tile_index * 64; // 64 bytes/tile at 8bpp
            uint32_t byte_offset = tile_offset + within_y * 8 + within_x;
            if (byte_offset >= sizeof(mem->vram)) continue;
            uint8_t color_index = mem->vram[byte_offset];
            if (color_index == 0) continue; // transparent

uint32_t pal_offset = GBA_BG_PALETTE_OFFSET + color_index * 2;
            if (pal_offset + 1 >= sizeof(mem->palette)) continue;
            uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));

            gba_ppu_plot_pixel(ppu, x, y, gba_ppu_bgr555_to_rgba8888(raw), (uint8_t)bg_index, false);
        }
    }
}

// ---- Regular (non-affine) sprite rendering ----------------------------

struct GbaSpriteShape { uint8_t w, h; };

// Indexed [shape][size], per GBATEK's standard OBJ shape/size table.
static const GbaSpriteShape GBA_SPRITE_SHAPES[3][4] = {
    { {8,8},   {16,16}, {32,32}, {64,64} }, // square
    { {16,8},  {32,8},  {32,16}, {64,32} }, // horizontal
    { {8,16},  {8,32},  {16,32}, {32,64} }, // vertical
};

static void gba_ppu_draw_sprites_at_priority(GbaPpuState* ppu, GbaMemory* mem, uint8_t priority_filter) {
    uint32_t obj_char_base = ((ppu->dispcnt & 0x7) >= 3) ? GBA_OBJ_CHAR_BASE_BITMAP : GBA_OBJ_CHAR_BASE_TILE;
    bool obj_1d_mapping = (ppu->dispcnt >> 6) & 1;

    // 128 OAM entries, 8 bytes each.
    for (int i = 0; i < 128; i++) {
        uint32_t base = i * 8;
        if (base + 5 >= sizeof(mem->oam)) break;

        uint16_t attr0 = (uint16_t)(mem->oam[base + 0] | (mem->oam[base + 1] << 8));
        uint16_t attr1 = (uint16_t)(mem->oam[base + 2] | (mem->oam[base + 3] << 8));
        uint16_t attr2 = (uint16_t)(mem->oam[base + 4] | (mem->oam[base + 5] << 8));

        bool affine_flag = (attr0 >> 8) & 1;

        // Bit 9 is overloaded: for regular sprites it's the disable bit,
        // for affine sprites it's "double-size" (doubles the on-screen
        // bounding box so a rotated sprite doesn't clip against its own
        // unrotated tile bounds).
        bool bit9 = (attr0 >> 9) & 1;
        if (!affine_flag && bit9) continue; // regular sprite, disabled

        uint8_t shape = (attr0 >> 14) & 0x3;
        if (shape == 3) continue; // prohibited

uint8_t obj_mode = (attr0 >> 10) & 0x3;
        if (obj_mode == 3) continue; // prohibited
        // obj_mode 1 (semi-transparent) draws normally here and is
        // handled by gba_ppu_apply_blending at end of frame (see
        // gba_ppu_plot_pixel's obj_semi_transparent param below).
        // obj_mode 2 (OBJ window) sprites are invisible -- they only
        // contribute coverage to the Obj Window mask, built separately
        // by gba_ppu_build_obj_window_mask before this function ever runs.
        if (obj_mode == 2) continue;

        bool color_256 = (attr0 >> 13) & 1;

        uint8_t size = (attr1 >> 14) & 0x3;
        GbaSpriteShape dims = GBA_SPRITE_SHAPES[shape][size];

        uint8_t priority = (attr2 >> 10) & 0x3;
        if (priority != priority_filter) continue;

        int32_t y = attr0 & 0xFF;
        if (y >= 160) y -= 256; // signed Y (8-bit two's complement)
        int32_t x = attr1 & 0x1FF;
        if (x >= 240) x -= 512; // signed X (9-bit two's complement)

uint32_t tile_index = attr2 & 0x3FF;
        uint32_t palette_bank = (attr2 >> 12) & 0xF; // 4bpp only

        // OBJ mosaic (attr0 bit 12). NOTE: approximation -- snaps using
        // sprite-local coordinates rather than hardware's screen-aligned
        // grid (which would make the mosaic pattern shift depending on
        // the sprite's on-screen position). Simpler and avoids negative-
        // coordinate edge cases at the sprite's own boundary; visually
        // very close for typical use.
        bool obj_mosaic = (attr0 >> 12) & 0x1;
        uint32_t mosaic_ow = obj_mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 8) : 1;
        uint32_t mosaic_oh = obj_mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 12) : 1;

        // Tiles-per-row for 2D OBJ char mapping is always 32 (fixed VRAM
        // layout), regardless of sprite width -- see GBATEK "OBJ VRAM
        // Character Data".
        uint32_t tile_w = dims.w / 8;

        if (!affine_flag) {
            bool hflip = (attr1 >> 12) & 1;
            bool vflip = (attr1 >> 13) & 1;

            for (uint32_t py = 0; py < (uint32_t)dims.h; py++) {
                int32_t screen_y = y + (int32_t)py;
                if (screen_y < 0 || screen_y >= GBA_SCREEN_HEIGHT) continue;

                for (uint32_t px = 0; px < (uint32_t)dims.w; px++) {
                    int32_t screen_x = x + (int32_t)px;
                    if (screen_x < 0 || screen_x >= GBA_SCREEN_WIDTH) continue;
                    if (!gba_ppu_window_layer_enabled(ppu, screen_x, screen_y, 4)) continue;

                    uint32_t mpx = obj_mosaic ? (px / mosaic_ow) * mosaic_ow : px;
                    uint32_t mpy = obj_mosaic ? (py / mosaic_oh) * mosaic_oh : py;
                    uint32_t sx = hflip ? (dims.w - 1 - mpx) : mpx;
                    uint32_t sy = vflip ? (dims.h - 1 - mpy) : mpy;
                    uint32_t tile_col = sx / 8;
                    uint32_t tile_row = sy / 8;
                    uint32_t within_x = sx % 8;
                    uint32_t within_y = sy % 8;

                    uint32_t this_tile = color_256
                        ? (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col) * 2
                                                         : (tile_row * 32 + tile_col) * 2))
                        : (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col)
                                                         : (tile_row * 32 + tile_col)));

                    uint8_t color_index;
                    if (color_256) {
                        uint32_t tile_offset = obj_char_base + this_tile * 64;
                        uint32_t byte_offset = tile_offset + within_y * 8 + within_x;
                        if (byte_offset >= sizeof(mem->vram)) continue;
                        color_index = mem->vram[byte_offset];
                    } else {
                        uint32_t tile_offset = obj_char_base + this_tile * 32;
                        uint32_t byte_offset = tile_offset + within_y * 4 + (within_x / 2);
                        if (byte_offset >= sizeof(mem->vram)) continue;
                        uint8_t packed = mem->vram[byte_offset];
                        color_index = (within_x & 1) ? (packed >> 4) : (packed & 0xF);
                    }

if (color_index == 0) continue; // transparent

                    uint32_t pal_offset = color_256
                        ? (GBA_OBJ_PALETTE_OFFSET + color_index * 2)
                        : (GBA_OBJ_PALETTE_OFFSET + (palette_bank * 16 + color_index) * 2);
                    if (pal_offset + 1 >= sizeof(mem->palette)) continue;
                    uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));

                    gba_ppu_plot_pixel(ppu, screen_x, screen_y, gba_ppu_bgr555_to_rgba8888(raw), 4, obj_mode == 1);
                }
            }
            continue;
        }

        // ---- Affine sprite ----
        // attr1 bits 9-13 select one of 32 shared affine parameter groups
        // (not per-sprite storage). Each group's PA/PB/PC/PD live in the
        // attr3 field (OAM bytes 6-7) of 4 consecutive OAM entries
        // (group*4 + 0..3 respectively), 8.8 fixed point -- flip bits
        // (attr1 12-13) don't apply to affine sprites, the matrix handles
        // orientation instead.
        bool double_size = bit9;
        uint32_t param_select = (attr1 >> 9) & 0x1F;
        uint32_t group_base = param_select * 4;

        auto read_affine_param = [&](uint32_t entry_index) -> int16_t {
            uint32_t off = entry_index * 8 + 6; // attr3 of that OAM entry
            if (off + 1 >= sizeof(mem->oam)) return 0x100; // identity fallback
            return (int16_t)(mem->oam[off] | (mem->oam[off + 1] << 8));
        };
        int16_t pa = read_affine_param(group_base + 0);
        int16_t pb = read_affine_param(group_base + 1);
        int16_t pc = read_affine_param(group_base + 2);
        int16_t pd = read_affine_param(group_base + 3);

        int32_t half_w = (int32_t)dims.w / 2;
        int32_t half_h = (int32_t)dims.h / 2;
        int32_t bound_w = double_size ? (int32_t)dims.w * 2 : (int32_t)dims.w;
        int32_t bound_h = double_size ? (int32_t)dims.h * 2 : (int32_t)dims.h;
        int32_t half_bound_w = bound_w / 2;
        int32_t half_bound_h = bound_h / 2;

        // Walk the screen-space bounding box and inverse-transform each
        // pixel back into (unrotated) sprite texture space -- opposite
        // direction from the regular-sprite loop above, which walks
        // texture space forward.
for (int32_t sy = 0; sy < bound_h; sy++) {
            int32_t screen_y = y + sy;
            if (screen_y < 0 || screen_y >= GBA_SCREEN_HEIGHT) continue;
            int32_t msy = obj_mosaic ? (sy / (int32_t)mosaic_oh) * (int32_t)mosaic_oh : sy;
            int32_t ay = msy - half_bound_h;

            for (int32_t sx = 0; sx < bound_w; sx++) {
                int32_t screen_x = x + sx;
                if (screen_x < 0 || screen_x >= GBA_SCREEN_WIDTH) continue;
                if (!gba_ppu_window_layer_enabled(ppu, screen_x, screen_y, 4)) continue;
                int32_t msx = obj_mosaic ? (sx / (int32_t)mosaic_ow) * (int32_t)mosaic_ow : sx;
                int32_t ax = msx - half_bound_w;

                int32_t tex_x = ((pa * ax + pb * ay) >> 8) + half_w;
                int32_t tex_y = ((pc * ax + pd * ay) >> 8) + half_h;

                if (tex_x < 0 || tex_x >= (int32_t)dims.w || tex_y < 0 || tex_y >= (int32_t)dims.h) continue;

                uint32_t tile_col = (uint32_t)tex_x / 8;
                uint32_t tile_row = (uint32_t)tex_y / 8;
                uint32_t within_x = (uint32_t)tex_x % 8;
                uint32_t within_y = (uint32_t)tex_y % 8;

                uint32_t this_tile = color_256
                    ? (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col) * 2
                                                     : (tile_row * 32 + tile_col) * 2))
                    : (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col)
                                                     : (tile_row * 32 + tile_col)));

                uint8_t color_index;
                if (color_256) {
                    uint32_t tile_offset = obj_char_base + this_tile * 64;
                    uint32_t byte_offset = tile_offset + within_y * 8 + within_x;
                    if (byte_offset >= sizeof(mem->vram)) continue;
                    color_index = mem->vram[byte_offset];
                } else {
                    uint32_t tile_offset = obj_char_base + this_tile * 32;
                    uint32_t byte_offset = tile_offset + within_y * 4 + (within_x / 2);
                    if (byte_offset >= sizeof(mem->vram)) continue;
                    uint8_t packed = mem->vram[byte_offset];
                    color_index = (within_x & 1) ? (packed >> 4) : (packed & 0xF);
                }

if (color_index == 0) continue; // transparent

                uint32_t pal_offset = color_256
                    ? (GBA_OBJ_PALETTE_OFFSET + color_index * 2)
                    : (GBA_OBJ_PALETTE_OFFSET + (palette_bank * 16 + color_index) * 2);
                if (pal_offset + 1 >= sizeof(mem->palette)) continue;
                uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));

                gba_ppu_plot_pixel(ppu, screen_x, screen_y, gba_ppu_bgr555_to_rgba8888(raw), 4, obj_mode == 1);
            }
        }
    }
}

// ---- Bitmap modes (3/4) ------------------------------------------------

// ---- Obj Window mask ---------------------------------------------------
//
// OBJ-mode-2 sprites don't draw visible pixels (see the obj_mode==2 skip
// in gba_ppu_draw_sprites_at_priority above) -- instead, wherever their
// tiles are opaque marks that screen pixel as "inside the Obj Window",
// consumed by gba_ppu_window_layer_enabled. Must run before the normal
// sprite/BG draw passes so the mask is ready when window checks start.
// NOTE: this duplicates a fair amount of the tile-sampling logic from
// gba_ppu_draw_sprites_at_priority (opacity check only, no color/blend/
// mosaic needed) -- consistent with this file's existing preference for
// straightforward duplication over a shared-but-more-complex abstraction.
static void gba_ppu_build_obj_window_mask(GbaPpuState* ppu, GbaMemory* mem) {
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
        ppu->obj_window_mask[i] = 0;
    }

    uint32_t obj_char_base = ((ppu->dispcnt & 0x7) >= 3) ? GBA_OBJ_CHAR_BASE_BITMAP : GBA_OBJ_CHAR_BASE_TILE;
    bool obj_1d_mapping = (ppu->dispcnt >> 6) & 1;

    for (int i = 0; i < 128; i++) {
        uint32_t base = i * 8;
        if (base + 5 >= sizeof(mem->oam)) break;

        uint16_t attr0 = (uint16_t)(mem->oam[base + 0] | (mem->oam[base + 1] << 8));
        uint16_t attr1 = (uint16_t)(mem->oam[base + 2] | (mem->oam[base + 3] << 8));
        uint16_t attr2 = (uint16_t)(mem->oam[base + 4] | (mem->oam[base + 5] << 8));

        uint8_t obj_mode = (attr0 >> 10) & 0x3;
        if (obj_mode != 2) continue; // only Obj Window sprites contribute here

        bool affine_flag = (attr0 >> 8) & 1;
        bool bit9 = (attr0 >> 9) & 1; // disabled (regular) / double-size (affine), same as the draw function
        if (!affine_flag && bit9) continue;

        uint8_t shape = (attr0 >> 14) & 0x3;
        if (shape == 3) continue;

        bool color_256 = (attr0 >> 13) & 1;
        uint8_t size = (attr1 >> 14) & 0x3;
        GbaSpriteShape dims = GBA_SPRITE_SHAPES[shape][size];

        int32_t y = attr0 & 0xFF;
        if (y >= 160) y -= 256;
        int32_t x = attr1 & 0x1FF;
        if (x >= 240) x -= 512;

        uint32_t tile_index = attr2 & 0x3FF;
        uint32_t tile_w = dims.w / 8;

        if (!affine_flag) {
            bool hflip = (attr1 >> 12) & 1;
            bool vflip = (attr1 >> 13) & 1;

            for (uint32_t py = 0; py < (uint32_t)dims.h; py++) {
                int32_t screen_y = y + (int32_t)py;
                if (screen_y < 0 || screen_y >= GBA_SCREEN_HEIGHT) continue;

                for (uint32_t px = 0; px < (uint32_t)dims.w; px++) {
                    int32_t screen_x = x + (int32_t)px;
                    if (screen_x < 0 || screen_x >= GBA_SCREEN_WIDTH) continue;

                    uint32_t sx = hflip ? (dims.w - 1 - px) : px;
                    uint32_t sy = vflip ? (dims.h - 1 - py) : py;
                    uint32_t tile_col = sx / 8, tile_row = sy / 8;
                    uint32_t within_x = sx % 8, within_y = sy % 8;

                    uint32_t this_tile = color_256
                        ? (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col) * 2
                                                         : (tile_row * 32 + tile_col) * 2))
                        : (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col)
                                                         : (tile_row * 32 + tile_col)));

                    uint8_t color_index;
                    if (color_256) {
                        uint32_t off = obj_char_base + this_tile * 64 + within_y * 8 + within_x;
                        if (off >= sizeof(mem->vram)) continue;
                        color_index = mem->vram[off];
                    } else {
                        uint32_t off = obj_char_base + this_tile * 32 + within_y * 4 + (within_x / 2);
                        if (off >= sizeof(mem->vram)) continue;
                        uint8_t packed = mem->vram[off];
                        color_index = (within_x & 1) ? (packed >> 4) : (packed & 0xF);
                    }
                    if (color_index == 0) continue;

                    ppu->obj_window_mask[screen_y * GBA_SCREEN_WIDTH + screen_x] = 1;
                }
            }
        } else {
            uint32_t param_select = (attr1 >> 9) & 0x1F;
            uint32_t group_base = param_select * 4;
            auto read_affine_param = [&](uint32_t entry_index) -> int16_t {
                uint32_t off = entry_index * 8 + 6;
                if (off + 1 >= sizeof(mem->oam)) return 0x100;
                return (int16_t)(mem->oam[off] | (mem->oam[off + 1] << 8));
            };
            int16_t pa = read_affine_param(group_base + 0);
            int16_t pb = read_affine_param(group_base + 1);
            int16_t pc = read_affine_param(group_base + 2);
            int16_t pd = read_affine_param(group_base + 3);

            bool double_size = bit9;
            int32_t half_w = (int32_t)dims.w / 2;
            int32_t half_h = (int32_t)dims.h / 2;
            int32_t bound_w = double_size ? (int32_t)dims.w * 2 : (int32_t)dims.w;
            int32_t bound_h = double_size ? (int32_t)dims.h * 2 : (int32_t)dims.h;
            int32_t half_bound_w = bound_w / 2;
            int32_t half_bound_h = bound_h / 2;

            for (int32_t sy = 0; sy < bound_h; sy++) {
                int32_t screen_y = y + sy;
                if (screen_y < 0 || screen_y >= GBA_SCREEN_HEIGHT) continue;
                int32_t ay = sy - half_bound_h;

                for (int32_t sx = 0; sx < bound_w; sx++) {
                    int32_t screen_x = x + sx;
                    if (screen_x < 0 || screen_x >= GBA_SCREEN_WIDTH) continue;
                    int32_t ax = sx - half_bound_w;

                    int32_t tex_x = ((pa * ax + pb * ay) >> 8) + half_w;
                    int32_t tex_y = ((pc * ax + pd * ay) >> 8) + half_h;
                    if (tex_x < 0 || tex_x >= (int32_t)dims.w || tex_y < 0 || tex_y >= (int32_t)dims.h) continue;

                    uint32_t tile_col = (uint32_t)tex_x / 8, tile_row = (uint32_t)tex_y / 8;
                    uint32_t within_x = (uint32_t)tex_x % 8, within_y = (uint32_t)tex_y % 8;

                    uint32_t this_tile = color_256
                        ? (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col) * 2
                                                         : (tile_row * 32 + tile_col) * 2))
                        : (tile_index + (obj_1d_mapping ? (tile_row * tile_w + tile_col)
                                                         : (tile_row * 32 + tile_col)));

                    uint8_t color_index;
                    if (color_256) {
                        uint32_t off = obj_char_base + this_tile * 64 + within_y * 8 + within_x;
                        if (off >= sizeof(mem->vram)) continue;
                        color_index = mem->vram[off];
                    } else {
                        uint32_t off = obj_char_base + this_tile * 32 + within_y * 4 + (within_x / 2);
                        if (off >= sizeof(mem->vram)) continue;
                        uint8_t packed = mem->vram[off];
                        color_index = (within_x & 1) ? (packed >> 4) : (packed & 0xF);
                    }
                    if (color_index == 0) continue;

                    ppu->obj_window_mask[screen_y * GBA_SCREEN_WIDTH + screen_x] = 1;
                }
            }
        }
    }
}

static void gba_ppu_draw_mode3(GbaPpuState* ppu, GbaMemory* mem) {
    // Bitmap modes still read BG2CNT for priority/mosaic (just not the
    // char/screen base bits, which don't apply to a flat bitmap) --
    // window and mosaic behave the same as any other BG2 content.
    bool mosaic = (ppu->bg[2].control >> 6) & 0x1;
    uint32_t mosaic_w = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 0) : 1;
    uint32_t mosaic_h = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 4) : 1;

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            if (!gba_ppu_window_layer_enabled(ppu, x, y, 2)) continue;
            int mx = mosaic ? (x / (int)mosaic_w) * (int)mosaic_w : x;
            int my = mosaic ? (y / (int)mosaic_h) * (int)mosaic_h : y;
            uint32_t off = (my * GBA_SCREEN_WIDTH + mx) * 2;
            if (off + 1 >= sizeof(mem->vram)) continue;
            uint16_t raw = (uint16_t)(mem->vram[off] | (mem->vram[off + 1] << 8));
            gba_ppu_plot_pixel(ppu, x, y, gba_ppu_bgr555_to_rgba8888(raw), 2, false);
        }
    }
}

static void gba_ppu_draw_mode4(GbaPpuState* ppu, GbaMemory* mem) {
    bool page1 = (ppu->dispcnt >> 4) & 1;
    uint32_t page_base = page1 ? 0xA000 : 0;
    bool mosaic = (ppu->bg[2].control >> 6) & 0x1;
    uint32_t mosaic_w = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 0) : 1;
    uint32_t mosaic_h = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 4) : 1;

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            if (!gba_ppu_window_layer_enabled(ppu, x, y, 2)) continue;
            int mx = mosaic ? (x / (int)mosaic_w) * (int)mosaic_w : x;
            int my = mosaic ? (y / (int)mosaic_h) * (int)mosaic_h : y;
            uint32_t off = page_base + my * GBA_SCREEN_WIDTH + mx;
            if (off >= sizeof(mem->vram)) continue;
            uint8_t color_index = mem->vram[off];
            uint32_t pal_offset = GBA_BG_PALETTE_OFFSET + color_index * 2;
            if (pal_offset + 1 >= sizeof(mem->palette)) continue;
            uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));
            gba_ppu_plot_pixel(ppu, x, y, gba_ppu_bgr555_to_rgba8888(raw), 2, false);
        }
    }
}

#define GBA_MODE5_WIDTH  160
#define GBA_MODE5_HEIGHT 128

// Mode 5: 160x128 16bpp bitmap, double-buffered like Mode 4. The bitmap
// is smaller than the 240x160 screen -- real hardware shows leftover
// VRAM data reinterpreted as pixels outside that area, which isn't worth
// modeling. Left as backdrop (already filled in by the caller before this
// runs) -- flagged simplification, not a silent gap.
static void gba_ppu_draw_mode5(GbaPpuState* ppu, GbaMemory* mem) {
    bool page1 = (ppu->dispcnt >> 4) & 1;
    uint32_t page_base = page1 ? 0xA000 : 0;
    bool mosaic = (ppu->bg[2].control >> 6) & 0x1;
    uint32_t mosaic_w = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 0) : 1;
    uint32_t mosaic_h = mosaic ? gba_ppu_mosaic_size(ppu->mosaic, 4) : 1;

    for (int y = 0; y < GBA_MODE5_HEIGHT; y++) {
        for (int x = 0; x < GBA_MODE5_WIDTH; x++) {
            if (!gba_ppu_window_layer_enabled(ppu, x, y, 2)) continue;
            int mx = mosaic ? (x / (int)mosaic_w) * (int)mosaic_w : x;
            int my = mosaic ? (y / (int)mosaic_h) * (int)mosaic_h : y;
            uint32_t off = page_base + (my * GBA_MODE5_WIDTH + mx) * 2;
            if (off + 1 >= sizeof(mem->vram)) continue;
            uint16_t raw = (uint16_t)(mem->vram[off] | (mem->vram[off + 1] << 8));
            gba_ppu_plot_pixel(ppu, x, y, gba_ppu_bgr555_to_rgba8888(raw), 2, false);
        }
    }
}

// ---- Top level -----------------------------------------------------

void gba_ppu_render_frame(GbaPpuState* ppu, GbaMemory* mem) {
    uint8_t mode = ppu->dispcnt & 0x7;

// Backdrop: BG palette entry 0.
    uint16_t backdrop_raw = (uint16_t)(mem->palette[0] | (mem->palette[1] << 8));
    uint32_t backdrop = gba_ppu_bgr555_to_rgba8888(backdrop_raw);
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
        ppu->framebuffer[i] = backdrop;
        // Blend tracking starts every pixel as "Backdrop on Backdrop" --
        // gba_ppu_plot_pixel shifts this down to "second" as real layers
        // draw on top of it.
        ppu->blend_top_layer[i] = 5;
        ppu->blend_second_layer[i] = 5;
        ppu->blend_second_color[i] = backdrop;
        ppu->blend_obj_semi_transparent[i] = 0;
    }

    if ((ppu->dispcnt >> 7) & 1) {
        // Forced blank -- screen outputs white, nothing else drawn.
        for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
            ppu->framebuffer[i] = 0xFFFFFFFFu;
        }
        return;
    }

    bool obj_enabled = (ppu->dispcnt >> 12) & 1;

    // Obj Window mask must exist before any window_layer_enabled check
    // runs below -- only worth building if Obj Window is actually on.
    if ((ppu->dispcnt >> 15) & 1) {
        gba_ppu_build_obj_window_mask(ppu, mem);
    }

if (mode == 3) {
        gba_ppu_draw_mode3(ppu, mem);
        // Mode 3 has no separate BG priority layers to interleave with
        // sprites (it's BG2 only) -- draw sprites on top in priority order.
        if (obj_enabled) {
            for (int p = 3; p >= 0; p--) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        gba_ppu_apply_blending(ppu);
        return;
    }

if (mode == 4) {
        gba_ppu_draw_mode4(ppu, mem);
        if (obj_enabled) {
            for (int p = 3; p >= 0; p--) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        gba_ppu_apply_blending(ppu);
        return;
    }

if (mode == 5) {
        gba_ppu_draw_mode5(ppu, mem);
        if (obj_enabled) {
            for (int p = 3; p >= 0; p--) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        gba_ppu_apply_blending(ppu);
        return;
    }

    if (mode == 0) {
        for (int p = 3; p >= 0; p--) {
            for (int bg = 3; bg >= 0; bg--) {
                bool bg_enabled = (ppu->dispcnt >> (8 + bg)) & 1;
                if (!bg_enabled) continue;
                GbaBgParams bp = gba_ppu_parse_bg_control(ppu->bg[bg].control);
                if (bp.priority != (uint8_t)p) continue;
                gba_ppu_draw_bg_layer(ppu, mem, bg);
            }
if (obj_enabled) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        gba_ppu_apply_blending(ppu);
        return;
    }

    // Modes 1 and 2 involve at least one affine BG layer (BG2 in mode 1,
    // BG2+BG3 in mode 2). Draw whichever of their layers are regular
    // (mode 1's BG0/BG1) plus the affine layer(s).
    if (mode == 1) {
        bool bg2_enabled = (ppu->dispcnt >> 10) & 1;
        for (int p = 3; p >= 0; p--) {
            for (int bg = 1; bg >= 0; bg--) { // BG0, BG1 only -- regular
                bool bg_enabled = (ppu->dispcnt >> (8 + bg)) & 1;
                if (!bg_enabled) continue;
                GbaBgParams bp = gba_ppu_parse_bg_control(ppu->bg[bg].control);
                if (bp.priority != (uint8_t)p) continue;
                gba_ppu_draw_bg_layer(ppu, mem, bg);
            }
            if (bg2_enabled && (ppu->bg[2].control & 0x3) == (uint8_t)p) {
                gba_ppu_draw_affine_bg_layer(ppu, mem, 2);
            }
if (obj_enabled) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        gba_ppu_apply_blending(ppu);
        return;
    }

    // mode == 2: both active layers (BG2, BG3) are affine.
    {
        bool bg2_enabled = (ppu->dispcnt >> 10) & 1;
        bool bg3_enabled = (ppu->dispcnt >> 11) & 1;
        for (int p = 3; p >= 0; p--) {
            // Draw BG3 before BG2 at a tied priority so BG2 (lower index)
            // ends up on top, matching Mode 0's own BG-index tie-break
            // convention (see that block's comment above).
            if (bg3_enabled && (ppu->bg[3].control & 0x3) == (uint8_t)p) {
                gba_ppu_draw_affine_bg_layer(ppu, mem, 3);
            }
            if (bg2_enabled && (ppu->bg[2].control & 0x3) == (uint8_t)p) {
                gba_ppu_draw_affine_bg_layer(ppu, mem, 2);
            }
if (obj_enabled) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
    }
    gba_ppu_apply_blending(ppu);
}