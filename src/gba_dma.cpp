#include "gba_dma.h"
#include "gba_memory.h"

// DMA implementation
//
// Plan:
//  - gba_dma_write_control: called when game writes DMAxCNT_H register,
//    parses timing/repeat/irq/size bits, if Immediate timing -> trigger now
//  - gba_dma_trigger: called by PPU (on VBlank/HBlank) or APU (FIFO low)
//    to fire channels waiting on that timing mode
//  - gba_dma_step: does the actual word-by-word copy for one transfer;
//    DMA1/2 in Special mode transfer exactly 4 words into APU FIFO per
//    trigger rather than the full word_count
//  - Lower channel number = higher priority if multiple trigger same cycle

// TODO: void gba_dma_init(GbaDmaState* dma)
// TODO: void gba_dma_write_control(GbaDmaState* dma, int channel, uint16_t value)
// TODO: void gba_dma_trigger(GbaDmaState* dma, int channel, GbaDmaTiming reason)
// TODO: void gba_dma_step(GbaDmaState* dma, GbaMemory* mem, int channel)
