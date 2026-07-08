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

typedef enum {
    GBA_DMA_DEST_INCREMENT       = 0,
    GBA_DMA_DEST_DECREMENT       = 1,
    GBA_DMA_DEST_FIXED           = 2,
    GBA_DMA_DEST_INCREMENT_RELOAD = 3 // increment, reset to start each repeat
} GbaDmaDestControl;

typedef enum {
    GBA_DMA_SRC_INCREMENT = 0,
    GBA_DMA_SRC_DECREMENT = 1,
    GBA_DMA_SRC_FIXED     = 2
    // 3 is prohibited per GBATEK
} GbaDmaSrcControl;

typedef struct {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t word_count;
    GbaDmaTiming timing;
    GbaDmaDestControl dest_control;
    GbaDmaSrcControl src_control;
    bool repeat;
    bool irq_enable;
    bool enabled;
    uint8_t unit_size; // 16 or 32 (bits)

    // Internal, not register-visible: current transfer position, so a
    // FIFO-refill (Special timing) can resume mid-run rather than
    // restarting src/dst from the register-mirrored start each call.
    uint32_t current_src;
    uint32_t current_dst;
    uint32_t words_remaining;
} GbaDmaChannel;

typedef struct {
    GbaDmaChannel channels[4];
} GbaDmaState;

void gba_dma_init(GbaDmaState* dma);

// Handles DMAxCNT register writes. Setting the enable bit on a channel
// with Immediate timing starts the transfer right away; other timings
// arm the channel to fire on gba_dma_trigger.
void gba_dma_write_control(GbaDmaState* dma, int channel, uint32_t value);

// Called by PPU (VBlank/HBlank) or APU/timers (Special, FIFO-driven) when
// a trigger condition occurs. No-ops if the channel isn't armed for that
// timing or isn't enabled.
void gba_dma_trigger(GbaDmaState* dma, int channel, GbaDmaTiming reason);

// Performs one pending transfer's worth of work: the full word_count for
// Immediate/VBlank/HBlank, or a single FIFO-sized chunk for Special
// (DMA1/2 audio refill) -- caller (gba_core_run_frame) owns which case
// applies based on how gba_dma_trigger was invoked.
void gba_dma_step(GbaDmaState* dma, struct GbaMemory* mem, int channel);

#ifdef __cplusplus
}
#endif

#endif // GBA_DMA_H
