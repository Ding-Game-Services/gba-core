#include "gba_cpu.h"
#include "gba_memory.h"

// Core CPU lifecycle + the mode-switch/exception plumbing that ties
// gba_cpu_arm.cpp and gba_cpu_thumb.cpp together.
//
// Plan:
//  - gba_cpu_init: zero everything, set sane defaults
//  - gba_cpu_reset: set PC to BIOS entry (0x00000000), mode = Supervisor,
//    IRQ/FIQ disabled, per GBATEK reset behavior
//  - gba_cpu_switch_mode: on mode change, swap r13/r14 (and r8-r12 if
//    entering/leaving FIQ) into/out of the active r[] view using the
//    banked arrays. This is the piece that has to be airtight -- get it
//    wrong and register state bleeds between modes silently.
//  - gba_cpu_enter_exception: save CPSR->SPSR for new mode, switch mode,
//    force ARM state (thumb_mode = false), disable IRQ (and FIQ if reset),
//    set LR to return address, jump PC to exception vector
//  - gba_cpu_step: check thumb_mode, dispatch to step_arm or step_thumb

void gba_cpu_init(GbaCpuState* cpu) {
    for (int i = 0; i < 16; i++) cpu->r[i] = 0;
    for (int i = 0; i < 5; i++) cpu->r8_12_fiq[i] = 0;
    for (int i = 0; i < 5; i++) cpu->r8_12_shared[i] = 0;
    for (int b = 0; b < GBA_BANK_COUNT; b++) {
        cpu->r13_14_banked[b][0] = 0;
        cpu->r13_14_banked[b][1] = 0;
        cpu->spsr[b] = 0;
    }
    cpu->cpsr = 0;
    cpu->thumb_mode = false;
    cpu->current_mode = GBA_MODE_SUPERVISOR;
}

void gba_cpu_reset(GbaCpuState* cpu) {
    gba_cpu_init(cpu);

    // Per GBATEK: reset enters Supervisor mode, ARM state, IRQ+FIQ disabled.
    cpu->current_mode = GBA_MODE_SUPERVISOR;
    cpu->thumb_mode = false;

    // CPSR: mode bits = Supervisor, I (bit 7) + F (bit 6) set = IRQ/FIQ disabled, T (bit 5) = 0 = ARM
    cpu->cpsr = (uint32_t)GBA_MODE_SUPERVISOR | (1u << 7) | (1u << 6);

    // PC starts at the reset vector (BIOS start)
    cpu->r[15] = 0x00000000;
}

// Maps a CPU mode to its bank index, or -1 for User/System (unbanked, share
// the same r13/r14 -- they're the only two non-privileged-distinct modes).
static int gba_cpu_bank_index(GbaCpuMode mode) {
    switch (mode) {
        case GBA_MODE_FIQ:        return GBA_BANK_FIQ;
        case GBA_MODE_SUPERVISOR: return GBA_BANK_SVC;
        case GBA_MODE_ABORT:      return GBA_BANK_ABT;
        case GBA_MODE_IRQ:        return GBA_BANK_IRQ;
        case GBA_MODE_UNDEFINED:  return GBA_BANK_UND;
        case GBA_MODE_USER:
        case GBA_MODE_SYSTEM:
        default:
            return -1;
    }
}

void gba_cpu_switch_mode(GbaCpuState* cpu, GbaCpuMode new_mode) {
    GbaCpuMode old_mode = cpu->current_mode;
    if (old_mode == new_mode) return;

    int old_bank = gba_cpu_bank_index(old_mode);
    int new_bank = gba_cpu_bank_index(new_mode);

    // Store current r13/r14 into the outgoing mode's bank (User/System share
    // no bank -- their r13/r14 live nowhere but r[] itself, so skip storing).
    if (old_bank != -1) {
        cpu->r13_14_banked[old_bank][0] = cpu->r[13];
        cpu->r13_14_banked[old_bank][1] = cpu->r[14];
    }

// R8-R12 are banked as a pair of sets: FIQ has its own, everyone else
    // (User/System/SVC/ABT/IRQ/UND) shares one set. Save the outgoing set,
    // load the incoming set.
    if (old_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r8_12_fiq[i] = cpu->r[8 + i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r8_12_shared[i] = cpu->r[8 + i];
    }

    if (new_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_fiq[i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_shared[i];
    }

    // Load new mode's r13/r14 into the active view.
    if (new_bank != -1) {
        cpu->r[13] = cpu->r13_14_banked[new_bank][0];
        cpu->r[14] = cpu->r13_14_banked[new_bank][1];
    }

cpu->current_mode = new_mode;
    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | (uint32_t)new_mode;
}

void gba_cpu_enter_exception(GbaCpuState* cpu, GbaCpuMode exception_mode, uint32_t vector_addr) {
    uint32_t old_cpsr = cpu->cpsr;
    bool was_thumb = cpu->thumb_mode;

// Return address offset depends on exception type (GBATEK). Assumes
    // r[15] holds the address of the *next* instruction to execute (not
    // yet adjusted for ARM's 2-stage prefetch) -- revisit once the actual
    // fetch/decode loop is wired up and we know exactly what r[15] holds
    // at the call site.
    //   IRQ: LR = PC + 4 (return to the instruction after the one that
    //        was interrupted, accounting for prefetch)
    //   SWI/UNDEFINED/ABORT: LR = PC (offset 0) -- not finalized yet,
    //        will need Thumb-specific adjustment later
    uint32_t return_offset = 0;
    if (exception_mode == GBA_MODE_IRQ) {
        return_offset = 4;
    }
    uint32_t return_addr = cpu->r[15] + return_offset;

    gba_cpu_switch_mode(cpu, exception_mode);

    // Save the CPSR as it was *before* switching, into the new mode's SPSR.
    int bank = gba_cpu_bank_index(exception_mode);
    if (bank != -1) {
        cpu->spsr[bank] = old_cpsr;
    }

    cpu->r[14] = return_addr;

    // Force ARM state on exception entry.
    cpu->thumb_mode = false;
    cpu->cpsr &= ~(1u << 5);

    // Disable IRQ on any exception entry. FIQ is only disabled on reset/FIQ
    // itself -- leave bit 6 untouched here for other exception types.
    cpu->cpsr |= (1u << 7);
    if (exception_mode == GBA_MODE_FIQ) {
        cpu->cpsr |= (1u << 6);
    }

    cpu->r[15] = vector_addr;

    (void)was_thumb; // not yet used -- will matter once fetch/decode tracks Thumb return offsets
}