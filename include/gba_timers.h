#ifndef GBA_TIMERS_H
#define GBA_TIMERS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 4 hardware timers (0-3), each a 16-bit counter.
//  - Prescaler: system clock / 1, /64, /256, or /1024 (selectable per timer)
//  - Cascade ("count-up") mode: timer increments when the PREVIOUS timer
//    (N-1) overflows, instead of its own prescaled clock. Not available
//    on Timer 0 (nothing precedes it).
//  - On overflow: reload from a 16-bit reload value, optionally fire IRQ,
//    and for Timer 0/1 specifically, optionally drain an APU Direct Sound
//    FIFO sample (see gba_apu.h / gba_dma.h Special timing).

typedef struct {
    uint16_t counter;
    uint16_t reload_value;
    uint16_t prescaler_select; // 0=/1, 1=/64, 2=/256, 3=/1024
    bool cascade_enable;
    bool irq_enable;
    bool enabled;
    uint32_t cycle_accumulator; // cycles banked toward next prescaled tick
} GbaTimer;

typedef struct {
    GbaTimer timers[4];
} GbaTimerState;

void gba_timers_init(GbaTimerState* t);

// Handles TMxCNT_H writes (prescaler/cascade/irq/start-stop bits).
void gba_timers_write_control(GbaTimerState* t, int index, uint16_t value);

// Handles TMxCNT_L writes -- sets reload_value only (does not affect the
// live counter until the next overflow-triggered reload, per GBATEK).
// ADDED beyond the original TODO list since the file's own plan comment
// calls for it; flag if you'd rather fold this into write_control instead.
void gba_timers_set_reload(GbaTimerState* t, int index, uint16_t value);

struct GbaInterruptState; // fwd decl, defined in gba_interrupts.h
struct GbaApuState;       // fwd decl, defined in gba_apu.h

// Advances all enabled, non-cascade timers by `cycles`, handles overflow
// (reload + optional IRQ + cascade notify to timer N+1). irq/apu are
// threaded through for overflow side effects (IRQ request, Direct Sound
// FIFO drain on Timer 0/1 overflow) -- caller (gba_core_run_frame) owns
// passing the real per-instance pointers.
void gba_timers_step(GbaTimerState* t, uint32_t cycles, struct GbaInterruptState* irq, struct GbaApuState* apu);

#ifdef __cplusplus
}
#endif

#endif // GBA_TIMERS_H
