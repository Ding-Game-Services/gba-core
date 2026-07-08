#ifndef GBA_INTERRUPTS_H
#define GBA_INTERRUPTS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// GBA has a single IRQ line into the CPU, fed by multiple sources ORed
// together in hardware. IE (Interrupt Enable) masks which sources can
// fire; IF (Interrupt Flag/request) marks which have fired; IME (master
// enable) gates all of them at once. Handler must check (IF & IE) to
// determine which source(s) triggered.

typedef enum {
    GBA_IRQ_VBLANK   = 1 << 0,
    GBA_IRQ_HBLANK   = 1 << 1,
    GBA_IRQ_VCOUNT   = 1 << 2,
    GBA_IRQ_TIMER0   = 1 << 3,
    GBA_IRQ_TIMER1   = 1 << 4,
    GBA_IRQ_TIMER2   = 1 << 5,
    GBA_IRQ_TIMER3   = 1 << 6,
    GBA_IRQ_SERIAL   = 1 << 7,
    GBA_IRQ_DMA0     = 1 << 8,
    GBA_IRQ_DMA1     = 1 << 9,
    GBA_IRQ_DMA2     = 1 << 10,
    GBA_IRQ_DMA3     = 1 << 11,
    GBA_IRQ_KEYPAD   = 1 << 12,
    GBA_IRQ_GAMEPAK  = 1 << 13
} GbaIrqSource;

typedef struct {
    uint16_t ie;    // Interrupt Enable register mirror
    uint16_t if_;   // Interrupt Flag/request register mirror (write-1-to-ack)
    bool ime;       // master enable
} GbaInterruptState;

void gba_interrupts_init(GbaInterruptState* irq);

// Called by PPU/timers/DMA/etc when a condition fires. ORs the source
// bit into IF; does not check IE/IME (that's gba_interrupts_check's job).
void gba_interrupts_request(GbaInterruptState* irq, GbaIrqSource source);

// Returns true if the CPU should enter the IRQ exception this step.
// NOTE: this checks IME and (IF & IE) only. The CPSR I-bit (CPU-side
// IRQ disable) is a separate gate that lives on GbaCpuState, not here --
// caller (gba_cpu_step) is expected to AND this result with !(cpsr & I-bit)
// before calling gba_cpu_enter_exception. Flagging this in case you'd
// rather this function take the CPU state and check both in one place.
bool gba_interrupts_check(const GbaInterruptState* irq);

// Handles write-1-to-acknowledge on the IF register: only the bits set
// in `value` are cleared, everything else in if_ is left alone.
void gba_interrupts_ack(GbaInterruptState* irq, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif // GBA_INTERRUPTS_H
