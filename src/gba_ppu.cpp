#include "gba_ppu.h"
#include "gba_memory.h"

// PPU implementation -- per-frame render pass (see gba_ppu.h for the
// per-scanline reminder/rationale).
//
// Plan:
//  - gba_ppu_init: zero state, allocate/clear framebuffer
//  - gba_ppu_render_frame: read DISPCNT to determine mode, dispatch to
//    per-mode background renderers (tile modes 0-2 vs bitmap modes 3-5),
//    then composite sprites (OAM) by priority, write result to framebuffer
//  - gba_ppu_get_framebuffer: expose pointer/copy for frontend to blit

// TODO: void gba_ppu_init(GbaPpuState* ppu)
// TODO: void gba_ppu_render_frame(GbaPpuState* ppu, GbaMemory* mem)
// TODO: const uint32_t* gba_ppu_get_framebuffer(GbaPpuState* ppu)
