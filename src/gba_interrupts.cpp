#include "gba_interrupts.h"

// Interrupts implementation
//
// Plan:
//  - gba_interrupts_request: OR the source bit into IF, called by any
//    hardware block (PPU on VBlank/HBlank/VCount match, timers on
//    overflow, DMA on transfer complete, keypad on button match, etc.)
//  - gba_interrupts_check: returns true if IME is set AND (IF & IE) != 0
//    AND CPU's own IRQ-disable (CPSR I bit) is clear -- this is the
//    gate gba_cpu_step() consults each instruction to decide whether to
//    call gba_cpu_enter_exception() with GBA_MODE_IRQ.
//  - gba_interrupts_ack: IF is write-1-to-clear (not write-the-value),
//    so writing a bit pattern clears only the matching set bits.

// TODO: void gba_interrupts_init(GbaInterruptState* irq)
// TODO: void gba_interrupts_request(GbaInterruptState* irq, GbaIrqSource source)
// TODO: bool gba_interrupts_check(GbaInterruptState* irq)
// TODO: void gba_interrupts_ack(GbaInterruptState* irq, uint16_t value)
