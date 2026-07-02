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

// TODO: struct GbaTimer
//   - uint16_t counter, reload_value
//   - uint16_t prescaler_select (0=1,1=64,2=256,3=1024)
//   - bool cascade_enable, irq_enable, enabled

// TODO: struct GbaTimerState { GbaTimer timers[4]; }

// TODO: gba_timers_init(...)
// TODO: gba_timers_write_control(...)   handles TMxCNT_H register writes
// TODO: gba_timers_step(GbaTimerState* t, uint32_t cycles)  advance all enabled timers, handle overflow/cascade/irq

#ifdef __cplusplus
}
#endif

#endif // GBA_TIMERS_H
