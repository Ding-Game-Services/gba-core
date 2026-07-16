#include "gba_ppu.h"
#include "gba_memory.h"
#include <cstring>

// PPU implementation -- per-frame render pass (see gba_ppu.h for the
// per-scanline reminder/rationale).
//
// SCOPE FOR THIS PASS (flagging cuts explicitly rather than leaving silent
// gaps):
//  - Implemented: Mode 0 (regular/non-affine tile BGs, all 4 layers),
//    Mode 3 (16bpp bitmap), Mode 4 (8bpp paletted bitmap, both pages),
//    regular (non-affine) sprites with shape/size/priority compositing.
//  - NOT implemented yet, each flagged at its hook point below: affine BG
//    layers (Mode 1's BG2, Mode 2's BG2/3), affine sprites, Mode 5,
//    mosaic, windows, alpha blending (OBJ semi-transparent mode falls
//    back to normal opaque draw for now).
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
};

static GbaBgParams gba_ppu_parse_bg_control(uint16_t control) {
    GbaBgParams p;
    p.priority    = control & 0x3;
    p.char_base   = ((control >> 2) & 0x3) * 0x4000;
    p.color_256   = ((control >> 7) & 0x1) != 0;
    p.screen_base = ((control >> 8) & 0x1F) * 0x800;
    p.screen_size = (control >> 14) & 0x3;
    // bit 6 (mosaic) intentionally ignored for now -- see file-top scope note.
    return p;
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

static void gba_ppu_draw_bg_layer(GbaPpuState* ppu, GbaMemory* mem, int bg_index) {
    GbaBgParams bp = gba_ppu_parse_bg_control(ppu->bg[bg_index].control);
    uint16_t scroll_x = ppu->bg[bg_index].scroll_x;
    uint16_t scroll_y = ppu->bg[bg_index].scroll_y;

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            bool opaque = false;
            uint32_t color = gba_ppu_sample_bg_pixel(mem, bp, x + scroll_x, y + scroll_y, &opaque);
            if (opaque) {
                ppu->framebuffer[y * GBA_SCREEN_WIDTH + x] = color;
            }
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
        if (affine_flag) continue; // TODO: affine sprites -- see file-top scope note

        bool disabled = (attr0 >> 9) & 1; // only meaningful when affine_flag==0
        if (disabled) continue;

        uint8_t shape = (attr0 >> 14) & 0x3;
        if (shape == 3) continue; // prohibited

        uint8_t obj_mode = (attr0 >> 10) & 0x3;
        if (obj_mode == 3) continue; // prohibited
        // obj_mode 1 (semi-transparent) and 2 (window) both fall back to a
        // normal opaque draw for now -- TODO, see file-top scope note.

        bool color_256 = (attr0 >> 13) & 1;

        uint8_t size = (attr1 >> 14) & 0x3;
        GbaSpriteShape dims = GBA_SPRITE_SHAPES[shape][size];

        uint8_t priority = (attr2 >> 10) & 0x3;
        if (priority != priority_filter) continue;

        int32_t y = attr0 & 0xFF;
        if (y >= 160) y -= 256; // signed Y (8-bit two's complement)
        int32_t x = attr1 & 0x1FF;
        if (x >= 240) x -= 512; // signed X (9-bit two's complement)

        bool hflip = (attr1 >> 12) & 1;
        bool vflip = (attr1 >> 13) & 1;
        uint32_t tile_index = attr2 & 0x3FF;
        uint32_t palette_bank = (attr2 >> 12) & 0xF; // 4bpp only

        // Tiles-per-row for 2D OBJ char mapping is always 32 (fixed VRAM
        // layout), regardless of sprite width -- see GBATEK "OBJ VRAM
        // Character Data".
        uint32_t tile_w = dims.w / 8;
        uint32_t tile_h = dims.h / 8;

        for (uint32_t py = 0; py < (uint32_t)dims.h; py++) {
            int32_t screen_y = y + (int32_t)py;
            if (screen_y < 0 || screen_y >= GBA_SCREEN_HEIGHT) continue;

            for (uint32_t px = 0; px < (uint32_t)dims.w; px++) {
                int32_t screen_x = x + (int32_t)px;
                if (screen_x < 0 || screen_x >= GBA_SCREEN_WIDTH) continue;

                uint32_t sx = hflip ? (dims.w - 1 - px) : px;
                uint32_t sy = vflip ? (dims.h - 1 - py) : py;
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

                ppu->framebuffer[screen_y * GBA_SCREEN_WIDTH + screen_x] = gba_ppu_bgr555_to_rgba8888(raw);
            }
        }
    }
}

// ---- Bitmap modes (3/4) ------------------------------------------------

static void gba_ppu_draw_mode3(GbaPpuState* ppu, GbaMemory* mem) {
    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            uint32_t off = (y * GBA_SCREEN_WIDTH + x) * 2;
            if (off + 1 >= sizeof(mem->vram)) continue;
            uint16_t raw = (uint16_t)(mem->vram[off] | (mem->vram[off + 1] << 8));
            ppu->framebuffer[y * GBA_SCREEN_WIDTH + x] = gba_ppu_bgr555_to_rgba8888(raw);
        }
    }
}

static void gba_ppu_draw_mode4(GbaPpuState* ppu, GbaMemory* mem) {
    bool page1 = (ppu->dispcnt >> 4) & 1;
    uint32_t page_base = page1 ? 0xA000 : 0;

    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GBA_SCREEN_WIDTH; x++) {
            uint32_t off = page_base + y * GBA_SCREEN_WIDTH + x;
            if (off >= sizeof(mem->vram)) continue;
            uint8_t color_index = mem->vram[off];
            uint32_t pal_offset = GBA_BG_PALETTE_OFFSET + color_index * 2;
            if (pal_offset + 1 >= sizeof(mem->palette)) continue;
            uint16_t raw = (uint16_t)(mem->palette[pal_offset] | (mem->palette[pal_offset + 1] << 8));
            ppu->framebuffer[y * GBA_SCREEN_WIDTH + x] = gba_ppu_bgr555_to_rgba8888(raw);
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
    }

    if ((ppu->dispcnt >> 7) & 1) {
        // Forced blank -- screen outputs white, nothing else drawn.
        for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
            ppu->framebuffer[i] = 0xFFFFFFFFu;
        }
        return;
    }

    bool obj_enabled = (ppu->dispcnt >> 12) & 1;

    if (mode == 3) {
        gba_ppu_draw_mode3(ppu, mem);
        // Mode 3 has no separate BG priority layers to interleave with
        // sprites (it's BG2 only) -- draw sprites on top in priority order.
        if (obj_enabled) {
            for (int p = 3; p >= 0; p--) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        return;
    }

    if (mode == 4) {
        gba_ppu_draw_mode4(ppu, mem);
        if (obj_enabled) {
            for (int p = 3; p >= 0; p--) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        return;
    }

    if (mode == 5) {
        // TODO: Mode 5 (160x128 16bpp bitmap, two pages) -- see file-top
        // scope note. Falls through to backdrop-only for now.
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
        return;
    }

    // Modes 1 and 2 involve at least one affine BG layer (BG2 in mode 1,
    // BG2+BG3 in mode 2) which isn't implemented yet -- see file-top scope
    // note. Draw whichever of their layers are regular (mode 1's BG0/BG1)
    // and leave the affine ones as backdrop for now.
    if (mode == 1) {
        for (int p = 3; p >= 0; p--) {
            for (int bg = 1; bg >= 0; bg--) { // BG0, BG1 only -- regular
                bool bg_enabled = (ppu->dispcnt >> (8 + bg)) & 1;
                if (!bg_enabled) continue;
                GbaBgParams bp = gba_ppu_parse_bg_control(ppu->bg[bg].control);
                if (bp.priority != (uint8_t)p) continue;
                gba_ppu_draw_bg_layer(ppu, mem, bg);
            }
            // TODO: BG2 affine layer for this priority level
            if (obj_enabled) {
                gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
            }
        }
        return;
    }

    // mode == 2: both active layers (BG2, BG3) are affine -- TODO entirely.
    if (obj_enabled) {
        for (int p = 3; p >= 0; p--) {
            gba_ppu_draw_sprites_at_priority(ppu, mem, (uint8_t)p);
        }
    }
}
