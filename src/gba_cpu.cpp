#include "gba_cpu.h"
#include "gba_memory.h"
#include "gba_interrupts.h"
#include <cstring>

// CPU lifecycle + dispatch layer. gba_cpu_arm.cpp / gba_cpu_thumb.cpp own
// the actual instruction interpreters (gba_cpu_step_arm/_thumb) and
// gba_cpu_check_condition; this file owns everything state-management
// around them: init/reset, mode-switch register banking, exception entry,
// and the top-level gba_cpu_step that picks ARM vs Thumb and checks for
// a pending IRQ first.
//
// PC CONVENTION (matches gba_cpu_arm.cpp's own note): r[15] is kept at
// (current_instruction_addr + 4), NOT the full ARM +8 pipeline offset --
// that's flagged there as not-yet-implemented for operand reads. This
// file's exception entry uses r[15] as-is for the saved return address
// (LR), which is what gba_cpu_step_arm's own SWI handling already
// assumes. IRQ entry technically wants a slightly different offset than
// SWI on real hardware (interrupt sampled between instructions vs. an
// explicit SWI opcode) -- treating them the same here is a known
// approximation consistent with this project's "coarse, not
// wait-state-accurate" cycle model.

// Maps a privileged exception-capable mode to its banked-register index.
// Returns -1 for User/System, which don't have banked r13/r14/SPSR (they
// share r13_14_user and have no SPSR at all).
static int gba_cpu_bank_for_mode(GbaCpuMode mode) {
    switch (mode) {
        case GBA_MODE_FIQ:        return GBA_BANK_FIQ;
        case GBA_MODE_SUPERVISOR: return GBA_BANK_SVC;
        case GBA_MODE_ABORT:      return GBA_BANK_ABT;
        case GBA_MODE_IRQ:        return GBA_BANK_IRQ;
        case GBA_MODE_UNDEFINED:  return GBA_BANK_UND;
        default:                  return -1; // USER / SYSTEM
    }
}

void gba_cpu_init(GbaCpuState* cpu) {
    std::memset(cpu, 0, sizeof(GbaCpuState));
    gba_cpu_reset(cpu);
}

void gba_cpu_reset(GbaCpuState* cpu) {
    std::memset(cpu->r, 0, sizeof(cpu->r));
    std::memset(cpu->r8_12_fiq, 0, sizeof(cpu->r8_12_fiq));
    std::memset(cpu->r8_12_shared, 0, sizeof(cpu->r8_12_shared));
    std::memset(cpu->r13_14_banked, 0, sizeof(cpu->r13_14_banked));
    std::memset(cpu->r13_14_user, 0, sizeof(cpu->r13_14_user));
    std::memset(cpu->spsr, 0, sizeof(cpu->spsr));

    // Real hardware reset state: Supervisor mode, ARM state, IRQ+FIQ
    // disabled, PC = 0x00000000 (BIOS reset vector). Matches gba_bios.h's
    // documented plan of executing real BIOS code at reset rather than an
    // HLE skip-straight-to-ROM shortcut.
    cpu->current_mode = GBA_MODE_SUPERVISOR;
    cpu->thumb_mode = false;
    cpu->cpsr = (uint32_t)GBA_MODE_SUPERVISOR | CPSR_I_BIT | CPSR_F_BIT;
    cpu->r[15] = 0x00000000u;
}

void gba_cpu_switch_mode(GbaCpuState* cpu, GbaCpuMode new_mode) {
    GbaCpuMode old_mode = cpu->current_mode;
    if (new_mode == old_mode) return; // already there, nothing to bank

    // Save outgoing r13 (SP) / r14 (LR).
    if (old_mode == GBA_MODE_USER || old_mode == GBA_MODE_SYSTEM) {
        cpu->r13_14_user[0] = cpu->r[13];
        cpu->r13_14_user[1] = cpu->r[14];
    } else {
        int bank = gba_cpu_bank_for_mode(old_mode);
        if (bank >= 0) {
            cpu->r13_14_banked[bank][0] = cpu->r[13];
            cpu->r13_14_banked[bank][1] = cpu->r[14];
        }
    }

    // Save outgoing r8-r12 -- FIQ has its own private copies, every other
    // mode (including User/System) shares one set.
    if (old_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r8_12_fiq[i] = cpu->r[8 + i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r8_12_shared[i] = cpu->r[8 + i];
    }

    // Load incoming r13/r14.
    if (new_mode == GBA_MODE_USER || new_mode == GBA_MODE_SYSTEM) {
        cpu->r[13] = cpu->r13_14_user[0];
        cpu->r[14] = cpu->r13_14_user[1];
    } else {
        int bank = gba_cpu_bank_for_mode(new_mode);
        if (bank >= 0) {
            cpu->r[13] = cpu->r13_14_banked[bank][0];
            cpu->r[14] = cpu->r13_14_banked[bank][1];
        }
    }

    // Load incoming r8-r12.
    if (new_mode == GBA_MODE_FIQ) {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_fiq[i];
    } else {
        for (int i = 0; i < 5; i++) cpu->r[8 + i] = cpu->r8_12_shared[i];
    }

    cpu->current_mode = new_mode;
    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | ((uint32_t)new_mode & 0x1Fu);
}

void gba_cpu_enter_exception(GbaCpuState* cpu, GbaCpuMode exception_mode, uint32_t vector_addr) {
    uint32_t old_cpsr = cpu->cpsr;
    uint32_t return_addr = cpu->r[15]; // see file-top PC CONVENTION note

    gba_cpu_switch_mode(cpu, exception_mode);

    int bank = gba_cpu_bank_for_mode(exception_mode);
    if (bank >= 0) {
        cpu->spsr[bank] = old_cpsr;
    }

    cpu->r[14] = return_addr;
    cpu->r[15] = vector_addr;

    cpu->cpsr |= CPSR_I_BIT; // exception entry always disables further IRQs
    if (exception_mode == GBA_MODE_FIQ) {
        cpu->cpsr |= CPSR_F_BIT; // FIQ entry also disables further FIQs
    }
    cpu->cpsr &= ~0x20u; // exceptions always enter ARM state
    cpu->thumb_mode = false;
}

uint32_t gba_cpu_step(GbaCpuState* cpu, GbaMemory* mem, GbaInterruptState* irq) {
    // IRQ is sampled once per instruction boundary here (not mid-pipeline
    // -- see file-top PC CONVENTION note). CPSR I-bit gate is checked
    // here, not inside gba_interrupts_check, per that function's own
    // header comment.
    if (irq != nullptr && !(cpu->cpsr & CPSR_I_BIT) && gba_interrupts_check(irq)) {
        gba_cpu_enter_exception(cpu, GBA_MODE_IRQ, 0x18);
        return 3; // exception entry: pipeline refill, approximated as 2S+1
    }

    return cpu->thumb_mode ? gba_cpu_step_thumb(cpu, mem) : gba_cpu_step_arm(cpu, mem);
}
