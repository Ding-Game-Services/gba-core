#include "gba_timers.h"

// Timers implementation
//
// Plan:
//  - gba_timers_write_control: parse TMxCNT_H writes (prescaler, cascade,
//    irq enable, start/stop). Writing TMxCNT_L sets reload_value.
//  - gba_timers_step: for each enabled, non-cascade timer, accumulate
//    cycles against its prescaler and increment counter. On overflow:
//    reload from reload_value, fire IRQ if enabled, notify Timer N+1 if
//    it's in cascade mode, and if this is Timer 0 or 1, notify APU FIFO
//    (see gba_apu.h) in case Direct Sound is using it as sample clock.
//  - Cascade timers don't consume their own prescaler cycles -- they only
//    advance on the preceding timer's overflow signal.

// TODO: void gba_timers_init(GbaTimerState* t)
// TODO: void gba_timers_write_control(GbaTimerState* t, int index, uint16_t value)
// TODO: void gba_timers_step(GbaTimerState* t, uint32_t cycles)
