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
//
// SIGNATURE GAP (flagging, not silently working around): gba_dma_trigger
// takes no GbaMemory*, so it cannot perform the copy itself -- only
// gba_dma_step can, and that needs `mem`. So trigger() here is just an
// eligibility gate (armed + timing matches). The actual contract is:
// caller (gba_core_run_frame) calls gba_dma_trigger(...) to decide IF a
// channel should move, then calls gba_dma_step(dma, mem, channel) itself
// right after. Same story for Immediate timing out of write_control --
// write_control has no `mem` either, so it only arms the channel; the
// caller is expected to follow an enabled Immediate-timing write with an
// explicit gba_dma_trigger(dma, ch, GBA_DMA_TIMING_IMMEDIATE) +
// gba_dma_step(dma, mem, ch). Flag if you'd rather thread `mem` through
// trigger/write_control directly instead.
//
// Also assumed: src_addr/dst_addr/word_count on GbaDmaChannel are written
// by the memory IO layer on DMAxSAD/DMAxDAD/DMAxCNT_L writes *before* the
// DMAxCNT_H (control) write that calls gba_dma_write_control -- matches
// real register order on hardware. This file never sets those three
// fields itself, only current_src/current_dst/words_remaining (its own
// working copies).
//
// IRQ note: neither trigger() nor step() gets a GbaInterruptState*, so
// firing GBA_IRQ_DMA0..3 on transfer completion is left to the caller --
// check ch->irq_enable after a channel that just hit words_remaining==0
// and auto-disabled.

void gba_dma_init(GbaDmaState* dma) {
    for (int i = 0; i < 4; i++) {
        GbaDmaChannel* ch = &dma->channels[i];
        ch->src_addr = 0;
        ch->dst_addr = 0;
        ch->word_count = 0;
        ch->timing = GBA_DMA_TIMING_IMMEDIATE;
        ch->dest_control = GBA_DMA_DEST_INCREMENT;
        ch->src_control = GBA_DMA_SRC_INCREMENT;
        ch->repeat = false;
        ch->irq_enable = false;
        ch->enabled = false;
        ch->unit_size = 16;
        ch->current_src = 0;
        ch->current_dst = 0;
        ch->words_remaining = 0;
    }
}

void gba_dma_write_control(GbaDmaState* dma, int channel, uint32_t value) {
    GbaDmaChannel* ch = &dma->channels[channel];

    // DMAxCNT_H bit layout (per GBATEK):
    //  5-6:  Dest Addr Control
    //  7-8:  Src Addr Control
    //  9:    Repeat
    //  10:   Transfer type (0=16bit, 1=32bit)
    //  11:   Game Pak DRQ (DMA3 only, not modeled yet)
    //  12-13: Start timing
    //  14:   IRQ enable
    //  15:   DMA Enable
    ch->dest_control = (GbaDmaDestControl)((value >> 5) & 0x3);
    ch->src_control   = (GbaDmaSrcControl)((value >> 7) & 0x3);
    ch->repeat        = ((value >> 9) & 0x1) != 0;
    ch->unit_size     = ((value >> 10) & 0x1) ? 32 : 16;
    ch->timing        = (GbaDmaTiming)((value >> 12) & 0x3);
    ch->irq_enable    = ((value >> 14) & 0x1) != 0;
    bool enable_bit   = ((value >> 15) & 0x1) != 0;

    bool was_enabled = ch->enabled;
    ch->enabled = enable_bit;

    if (enable_bit && !was_enabled) {
        // Freshly armed: snapshot start addresses/count into the working
        // (current_*) copies. src_addr/dst_addr/word_count are assumed
        // already up to date, see file-top note.
        ch->current_src = ch->src_addr;
        ch->current_dst = ch->dst_addr;
        ch->words_remaining = ch->word_count;
    }
}

void gba_dma_trigger(GbaDmaState* dma, int channel, GbaDmaTiming reason) {
    GbaDmaChannel* ch = &dma->channels[channel];
    if (!ch->enabled) return;
    if (ch->timing != reason) return;
    // Eligible -- see file-top SIGNATURE GAP note. Nothing to mutate here;
    // caller follows this with gba_dma_step(dma, mem, channel).
}

void gba_dma_step(GbaDmaState* dma, GbaMemory* mem, int channel) {
    GbaDmaChannel* ch = &dma->channels[channel];
    if (!ch->enabled || ch->words_remaining == 0) return;

    // Special timing (DMA1/2 Direct Sound FIFO refill) only ever moves one
    // FIFO-sized chunk (4 words) per call, not the whole remaining count.
    uint32_t transfer_count = ch->words_remaining;
    if (ch->timing == GBA_DMA_TIMING_SPECIAL) {
        transfer_count = (ch->words_remaining < 4) ? ch->words_remaining : 4;
    }

    const uint32_t unit_bytes = (ch->unit_size == 32) ? 4 : 2;

    for (uint32_t i = 0; i < transfer_count; i++) {
        if (ch->unit_size == 32) {
            uint32_t val = gba_mem_read32(mem, ch->current_src);
            gba_mem_write32(mem, ch->current_dst, val);
        } else {
            uint16_t val = gba_mem_read16(mem, ch->current_src);
            gba_mem_write16(mem, ch->current_dst, val);
        }

        switch (ch->src_control) {
            case GBA_DMA_SRC_INCREMENT: ch->current_src += unit_bytes; break;
            case GBA_DMA_SRC_DECREMENT: ch->current_src -= unit_bytes; break;
            case GBA_DMA_SRC_FIXED:     break;
        }

        switch (ch->dest_control) {
            case GBA_DMA_DEST_INCREMENT:
            case GBA_DMA_DEST_INCREMENT_RELOAD:
                ch->current_dst += unit_bytes;
                break;
            case GBA_DMA_DEST_DECREMENT:
                ch->current_dst -= unit_bytes;
                break;
            case GBA_DMA_DEST_FIXED:
                break;
        }

        ch->words_remaining--;
    }

    if (ch->words_remaining == 0) {
        if (ch->repeat && ch->timing != GBA_DMA_TIMING_IMMEDIATE) {
            // Repeat: word count reloads from the register value. Source
            // address is NOT reset (continues from where it left off, per
            // GBATEK) -- this matters for Direct Sound FIFO feeds. Dest
            // only resets to its original value in Increment/Reload mode.
            ch->words_remaining = ch->word_count;
            if (ch->dest_control == GBA_DMA_DEST_INCREMENT_RELOAD) {
                ch->current_dst = ch->dst_addr;
            }
        } else {
            ch->enabled = false;
        }
        // ch->irq_enable is left set for the caller to check and request
        // GBA_IRQ_DMA0..3 via GbaInterruptState -- see file-top IRQ note.
    }
}
