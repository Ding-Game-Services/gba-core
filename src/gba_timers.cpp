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

// Forward decls -- defined in gba_interrupts.h / gba_apu.h. Not included
// directly to avoid pulling those headers' full contents in here; caller
// (gba_core.cpp) is expected to wire the real pointers through once the
// core-level struct exists. For now these are declared extern "C" so the
// linker resolves them once those files exist, matching the pattern
// gba_cpu.cpp already uses for gba_cpu_step_arm/thumb.
struct GbaInterruptState;
struct GbaApuState;

extern "C" {
    void gba_interrupts_request_from_timer(struct GbaInterruptState* irq, int timer_index);
    void gba_apu_timer_tick(struct GbaApuState* apu, int fifo_index);
}

// Prescaler select (bits 0-1 of TMxCNT_H) -> clock divider, per GBATEK.
static uint32_t gba_timers_prescaler_divisor(uint16_t prescaler_select) {
    switch (prescaler_select & 0x3) {
        case 0: return 1;
        case 1: return 64;
        case 2: return 256;
        case 3: return 1024;
        default: return 1;
    }
}

void gba_timers_init(GbaTimerState* t) {
    for (int i = 0; i < 4; i++) {
        t->timers[i].counter = 0;
        t->timers[i].reload_value = 0;
        t->timers[i].prescaler_select = 0;
        t->timers[i].cascade_enable = false;
        t->timers[i].irq_enable = false;
        t->timers[i].enabled = false;
        t->timers[i].cycle_accumulator = 0;
    }
}

void gba_timers_write_control(GbaTimerState* t, int index, uint16_t value) {
    if (index < 0 || index >= 4) return;

    GbaTimer* timer = &t->timers[index];
    bool was_enabled = timer->enabled;

    timer->prescaler_select = value & 0x3;
    // Cascade bit is meaningless on Timer 0 (nothing precedes it) --
    // GBATEK marks this bit as unused/ignored for TM0CNT_H, so we don't
    // special-case it here beyond just never checking cascade for index 0
    // in gba_timers_step.
    timer->cascade_enable   = (value >> 2) & 0x1;
    timer->irq_enable       = (value >> 6) & 0x1;
    timer->enabled          = (value >> 7) & 0x1;

    // Per GBATEK: on the 0->1 transition of the enable bit, the counter
    // is reloaded from reload_value and the prescaler is reset. Simply
    // writing new control bits while already running does NOT reload.
    if (timer->enabled && !was_enabled) {
        timer->counter = timer->reload_value;
        timer->cycle_accumulator = 0;
    }
}

void gba_timers_set_reload(GbaTimerState* t, int index, uint16_t value) {
    if (index < 0 || index >= 4) return;
    t->timers[index].reload_value = value;
}

// Handles one timer's overflow: reload, optional IRQ, optional cascade
// notify to timer N+1, optional APU FIFO notify for timers 0/1.
static void gba_timers_handle_overflow(GbaTimerState* t, int index) {
    GbaTimer* timer = &t->timers[index];
    timer->counter = timer->reload_value;

    if (timer->irq_enable) {
        // IRQ source enum (GBA_IRQ_TIMER0..3) is bit-position-encoded
        // starting at index 3 in gba_interrupts.h -- caller-side wiring
        // (gba_interrupts_request_from_timer) owns translating timer
        // index to the correct GbaIrqSource bit and calling
        // gba_interrupts_request. Kept as an indirection here so this
        // file doesn't need to include gba_interrupts.h directly.
        gba_interrupts_request_from_timer(nullptr, index);
        // NOTE: passing nullptr for now -- gba_core.cpp will need to
        // replace this call with one that actually threads the real
        // GbaInterruptState* through gba_timers_step once the core-level
        // wiring exists. Flagging loudly since a nullptr deref here would
        // be an easy thing to miss until IRQs are actually exercised.
    }

    // Timers 0 and 1 can drive Direct Sound sample playback -- notify
    // the APU unconditionally on overflow; gba_apu_timer_tick itself is
    // responsible for checking whether SOUNDCNT_H actually has this timer
    // selected for FIFO A/B before doing anything.
    if (index == 0 || index == 1) {
        gba_apu_timer_tick(nullptr, index);
        // Same nullptr caveat as above -- real GbaApuState* needs to come
        // from gba_core.cpp's wiring once that exists.
    }

    // Cascade: notify the next timer up, if it exists and has cascade
    // enabled. Timer 3 has no successor.
    if (index < 3) {
        GbaTimer* next = &t->timers[index + 1];
        if (next->enabled && next->cascade_enable) {
            next->counter++;
            if (next->counter == 0) {
                // Cascaded timer itself overflowed -- recurse so a chain
                // of cascades (e.g. Timer 2 cascading into Timer 3) all
                // resolve within the same step call.
                gba_timers_handle_overflow(t, index + 1);
            }
        }
    }
}

void gba_timers_step(GbaTimerState* t, uint32_t cycles) {
    for (int i = 0; i < 4; i++) {
        GbaTimer* timer = &t->timers[i];

        if (!timer->enabled) continue;
        // Cascade-mode timers don't consume their own prescaled clock --
        // they only advance via gba_timers_handle_overflow's cascade
        // notify path above. Timer 0 ignores cascade_enable regardless
        // of what's written there (see write_control note).
        if (i != 0 && timer->cascade_enable) continue;

        uint32_t divisor = gba_timers_prescaler_divisor(timer->prescaler_select);
        timer->cycle_accumulator += cycles;

        while (timer->cycle_accumulator >= divisor) {
            timer->cycle_accumulator -= divisor;
            timer->counter++;
            if (timer->counter == 0) { // wrapped past 0xFFFF
                gba_timers_handle_overflow(t, i);
            }
        }
    }
}
