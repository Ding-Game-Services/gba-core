#include "gba_cpu.h"
#include "gba_memory.h"

// THUMB mode interpreter (16-bit opcodes)
//
// Plan:
//  - gba_cpu_step_thumb() fetches a 16-bit opcode, dispatches based on
//    the 19 THUMB instruction format groups (simpler/more regular than
//    ARM's decode space, no per-instruction condition codes except
//    conditional branch).
//  - BX instruction (and any Thumb opcode capable of it) can flip
//    cpu->thumb_mode mid-stream -- dispatch loop must re-check every step,
//    never assume mode stays constant between instructions.
//  - Same dispatch strategy question as ARM (table vs switch) -- keep
//    consistent with whatever we land on there.

// TODO: uint16_t gba_cpu_fetch_thumb(GbaCpuState* cpu, GbaMemory* mem)
// TODO: void gba_cpu_step_thumb(GbaCpuState* cpu, GbaMemory* mem)
