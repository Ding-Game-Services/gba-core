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

uint16_t gba_cpu_fetch_thumb(GbaCpuState* cpu, GbaMemory* mem) {
    return gba_memory_read16(mem, cpu->r[15]);
}

void gba_cpu_step_thumb(GbaCpuState* cpu, GbaMemory* mem) {
    uint16_t opcode = gba_cpu_fetch_thumb(cpu, mem);
    cpu->r[15] += 2;

    // TODO: decode into the 19 Thumb format groups
    switch ((opcode >> 13) & 0x7) {
        default:
            // stub - unimplemented group
            break;
    }
}
