#ifndef GBA_DMA_H
#define GBA_DMA_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// GBA has 4 DMA channels (0-3), each does CPU-free memory-to-memory
// transfers. Key differences per channel:
//  - DMA0: cannot access cart ROM (internal-only transfers)
//  - DMA1/DMA2: feed the APU Direct Sound FIFOs (see gba_apu.h) via
//    "Special" timing mode, triggered by FIFO running low
//  - DMA3: only channel that can write to cart Flash/EEPROM
// Trigger timing per channel: Immediate, VBlank, HBlank, or Special
// (channel-dependent meaning -- audio FIFO for DMA1/2).

typedef enum {
    GBA_DMA_TIMING_IMMEDIATE = 0,
    GBA_DMA_TIMING_VBLANK    = 1,
    GBA_DMA_TIMING_HBLANK    = 2,
    GBA_DMA_TIMING_SPECIAL   = 3
} GbaDmaTiming;

// TODO: struct GbaDmaChannel
//   - uint32_t src_addr, dst_addr, word_count
//   - GbaDmaTiming timing
//   - bool repeat, irq_enable, enabled
//   - uint8_t unit_size (16 or 32 bit transfers)

// TODO: struct GbaDmaState { GbaDmaChannel channels[4]; }

// TODO: gba_dma_init(...)
// TODO: gba_dma_write_control(...)   handles DMAxCNT register writes, may trigger start
// TODO: gba_dma_trigger(GbaDmaState* dma, int channel, GbaDmaTiming reason)
// TODO: gba_dma_step(...)            perform pending transfer (immediate or one FIFO refill)

#ifdef __cplusplus
}
#endif

#endif // GBA_DMA_H
