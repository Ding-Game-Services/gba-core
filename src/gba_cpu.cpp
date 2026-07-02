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

// TODO: void gba_cpu_init(GbaCpuState* cpu)
// TODO: void gba_cpu_reset(GbaCpuState* cpu)
// TODO: void gba_cpu_switch_mode(GbaCpuState* cpu, GbaCpuMode new_mode)
// TODO: void gba_cpu_enter_exception(GbaCpuState* cpu, GbaCpuMode exception_mode, uint32_t vector_addr)
// TODO: void gba_cpu_step(GbaCpuState* cpu, GbaMemory* mem)
