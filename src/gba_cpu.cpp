#include "gba_cpu.h"
#include "gba_memory.h"
#include "gba_interrupts.h"
#include <cstring>

// Ties gba_cpu_step_arm/gba_cpu_step_thumb together, handles mode
// switching/banking, and exception entry (SWI, hardware IRQ).

void gba_cpu_init(GbaCpuState* cpu) {
    std::memset(cpu, 0, sizeof(GbaCpuState));
    // Idle state -- gba_core_reset is what actually sets up the real boot
    // vector/mode/CPSR. This just guarantees a fully zeroed, well-defined
    // starting point (no uninitialized reads before reset runs).
    cpu->current_mode = GBA_MODE_SUPERVISOR;
    cpu->cpsr = (uint32_t)GBA_MODE_SUPERVISOR;
}

void gba_cpu_reset(GbaCpuState* cpu) {
    // No CPU-side state persists across a reset -- re-init is correct.
    // (gba_core_reset is responsible for the post-reset boot vector/mode.)
    gba_cpu_init(cpu);
}

// Maps a privileged mode to its bank index, or -1 for User/System (which
// share cpu->r13_14_user instead of an entry in r13_14_banked).
static int bank_index_for_mode(GbaCpuMode mode) {
    switch (mode) {
        case GBA_MODE_FIQ:        return GBA_BANK_FIQ;
        case GBA_MODE_SUPERVISOR: return GBA_BANK_SVC;
        case GBA_MODE_ABORT:      return GBA_BANK_ABT;
        case GBA_MODE_IRQ:        return GBA_BANK_IRQ;
        case GBA_MODE_UNDEFINED:  return GBA_BANK_UND;
        default:                  return -1; // USER / SYSTEM
    }
}

void gba_cpu_switch_mode(GbaCpuState* cpu, GbaCpuMode new_mode) {
    if (new_mode == cpu->current_mode) {
        return;
    }

    // --- Save outgoing mode's R8-R12 ---
    // Only FIQ has its own R8-R12 bank; every other mode (including
    // User/System) shares r8_12_shared.
    if (cpu->current_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r8_12_fiq[i] = cpu->r[8 + i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r8_12_shared[i] = cpu->r[8 + i];
    }

    // --- Save outgoing mode's R13/R14 ---
    int outgoing_bank = bank_index_for_mode(cpu->current_mode);
    if (outgoing_bank >= 0) {
        cpu->r13_14_banked[outgoing_bank][0] = cpu->r[13];
        cpu->r13_14_banked[outgoing_bank][1] = cpu->r[14];
    } else {
        cpu->r13_14_user[0] = cpu->r[13];
        cpu->r13_14_user[1] = cpu->r[14];
    }

    // --- Load incoming mode's R8-R12 ---
    if (new_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_fiq[i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_shared[i];
    }

    // --- Load incoming mode's R13/R14 ---
    int incoming_bank = bank_index_for_mode(new_mode);
    if (incoming_bank >= 0) {
        cpu->r[13] = cpu->r13_14_banked[incoming_bank][0];
        cpu->r[14] = cpu->r13_14_banked[incoming_bank][1];
    } else {
        cpu->r[13] = cpu->r13_14_user[0];
        cpu->r[14] = cpu->r13_14_user[1];
    }

    // Update CPSR mode bits (bottom 5) to match, keeping every other bit.
    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | (uint32_t)new_mode;
    cpu->current_mode = new_mode;
}

void gba_cpu_enter_exception(GbaCpuState* cpu, GbaCpuMode exception_mode, uint32_t vector_addr) {
    uint32_t old_cpsr = cpu->cpsr;
    uint32_t return_addr = cpu->r[15]; // caller (SWI/IRQ dispatch) is expected to have
                                        // r[15] already sitting at the correct return
                                        // value per the ARM exception model.

    gba_cpu_switch_mode(cpu, exception_mode);

    // Stash the pre-exception CPSR in the new mode's SPSR bank.
    int bank = bank_index_for_mode(exception_mode);
    if (bank >= 0) {
        cpu->spsr[bank] = old_cpsr;
    }

    cpu->r[14] = return_addr; // LR_<mode> = return address
    cpu->cpsr |= CPSR_I_BIT;  // exceptions always mask IRQ on entry
    if (exception_mode == GBA_MODE_SUPERVISOR) {
        // Reset/SWI additionally leave FIQ state untouched per spec, so no
        // F-bit change here; only the hardware Reset vector forces F too,
        // which isn't reached through this function.
    }
    cpu->thumb_mode = false; // exceptions always enter in ARM state
    cpu->cpsr &= ~0x20u;

    cpu->r[15] = vector_addr;
}

// Dispatches to the ARM or Thumb interpreter for one instruction, after
// checking for a pending hardware IRQ. Per gba_interrupts.h's own note:
// gba_interrupts_check only looks at IME/IE/IF -- the CPSR I-bit is a
// separate gate this function is responsible for ANDing in.
uint32_t gba_cpu_step(GbaCpuState* cpu, GbaMemory* mem, GbaInterruptState* irq) {
    bool irq_line_active = gba_interrupts_check(irq);
    bool irq_masked = (cpu->cpsr & CPSR_I_BIT) != 0;

    if (irq_line_active && !irq_masked) {
        // IRQ return address per ARM exception model: PC+4 from the next
        // instruction to be executed (i.e. current r[15], since ARM/Thumb
        // step functions haven't run yet this call -- r[15] here already
        // points at the not-yet-executed instruction, +4 more per spec).
        cpu->r[15] += 4;
        gba_cpu_enter_exception(cpu, GBA_MODE_IRQ, 0x18);
        return 3; // exception entry: rough fixed cost, same class as SWI/branch refill
    }

    return cpu->thumb_mode ? gba_cpu_step_thumb(cpu, mem)
                            : gba_cpu_step_arm(cpu, mem);
}
