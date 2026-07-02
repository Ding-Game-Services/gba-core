#include "gba_cpu.h"
#include "gba_memory.h"

// ARM mode interpreter (32-bit opcodes)
//
// Plan:
//  - gba_cpu_step_arm() fetches a 32-bit opcode, checks condition code
//    (top 4 bits), and dispatches based on the standard ARM decode groups:
//    Data Processing, Multiply, PSR Transfer, Single/Block Data Transfer,
//    Branch, Software Interrupt, etc.
//  - Dispatch strategy TBD: table-driven (opcode -> function pointer) vs
//    switch/if-chain on decoded bit groups. Leaning table-driven for
//    maintainability, revisit once we see how big the table gets.
//  - Each handler reads/writes GbaCpuState.r[] directly, flags via cpsr.

// TODO: uint32_t gba_cpu_fetch_arm(GbaCpuState* cpu, GbaMemory* mem)
// TODO: bool gba_cpu_check_condition(uint32_t cpsr, uint32_t cond_bits)
// TODO: void gba_cpu_step_arm(GbaCpuState* cpu, GbaMemory* mem)
