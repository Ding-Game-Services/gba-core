#include "gba_interrupts.h"

// Interrupts implementation
//
// Plan:
//  - gba_interrupts_request: OR the source bit into IF, called by any
//    hardware block (PPU on VBlank/HBlank/VCount match, timers on
//    overflow, DMA on transfer complete, keypad on button match, etc.)
//  - gba_interrupts_check: returns true if IME is set AND (IF & IE) != 0.
//    The CPSR I-bit (CPU-side IRQ disable) is NOT checked here -- that
//    bit lives on GbaCpuState, which this module doesn't include on
//    purpose to avoid a circular dependency (gba_cpu.cpp will call into
//    here, not the other way around). gba_cpu_step() is expected to AND
//    this result with its own I-bit check before calling
//    gba_cpu_enter_exception() with GBA_MODE_IRQ.
//  - gba_interrupts_ack: IF is write-1-to-clear (not write-the-value),
//    so writing a bit pattern clears only the matching set bits.

void gba_interrupts_init(GbaInterruptState* irq) {
    irq->ie = 0;
    irq->if_ = 0;
    irq->ime = false;
}

void gba_interrupts_request(GbaInterruptState* irq, GbaIrqSource source) {
    irq->if_ |= (uint16_t)source;
}

bool gba_interrupts_check(const GbaInterruptState* irq) {
    if (!irq->ime) {
        return false;
    }
    return (irq->if_ & irq->ie) != 0;
}

void gba_interrupts_ack(GbaInterruptState* irq, uint16_t value) {
    irq->if_ &= ~value;
}