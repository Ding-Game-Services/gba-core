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

uint16_t top3 = (opcode >> 13) & 0x7;

    switch (top3) {
        case 0x0: { // Formats 1 & 2 share this top-3 bits region
            uint16_t op = (opcode >> 11) & 0x3; // bits [12:11]

if (op == 0x3) {
                // Format 2: ADD/SUB register or immediate (3-bit operand)
                bool immediate = (opcode >> 10) & 0x1; // I bit
                bool subtract  = (opcode >> 9) & 0x1;  // Op bit
                uint32_t rn_or_imm = (opcode >> 6) & 0x7;
                uint32_t rs = (opcode >> 3) & 0x7;
                uint32_t rd = opcode & 0x7;

                uint32_t operand1 = cpu->r[rs];
                uint32_t operand2 = immediate ? rn_or_imm : cpu->r[rn_or_imm];

                uint32_t result;
                bool carry, overflow;
                if (subtract) {
                    result = operand1 - operand2;
                    carry = operand1 >= operand2; // no borrow
                    overflow = ((operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                } else {
                    result = operand1 + operand2;
                    carry = result < operand1; // unsigned overflow
                    overflow = (~(operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                }

                cpu->r[rd] = result;
                cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (carry ? FLAG_C : 0)
                    | (overflow ? FLAG_V : 0);
                // Format 2 always sets flags (no S bit -- implicit).
                break;
            }

            // Format 1: Move Shifted Register (LSL/LSR/ASR #imm5)
            uint32_t rd  = opcode & 0x7;
            uint32_t rs  = (opcode >> 3) & 0x7;
            uint32_t imm5 = (opcode >> 6) & 0x1F;
            uint32_t value = cpu->r[rs];
            bool carry_in = cpu->cpsr & FLAG_C;

            uint32_t result;
            bool carry_out;
            switch (op) {
                case 0x0: // LSL
                    if (imm5 == 0) {
                        result = value;
                        carry_out = carry_in;
                    } else {
                        carry_out = (value >> (32 - imm5)) & 0x1;
                        result = value << imm5;
                    }
                    break;
                case 0x1: // LSR
                    if (imm5 == 0) {
                        carry_out = (value >> 31) & 0x1; // LSR #0 means LSR #32
                        result = 0;
                    } else {
                        carry_out = (value >> (imm5 - 1)) & 0x1;
                        result = value >> imm5;
                    }
                    break;
                case 0x2: { // ASR
                    int32_t signed_value = (int32_t)value;
                    if (imm5 == 0) {
                        carry_out = (value >> 31) & 0x1; // ASR #0 means ASR #32
                        result = (uint32_t)(signed_value >> 31);
                    } else {
                        carry_out = (value >> (imm5 - 1)) & 0x1;
                        result = (uint32_t)(signed_value >> imm5);
                    }
                    break;
                }
                default:
                    result = value;
                    carry_out = carry_in;
                    break;
            }

            cpu->r[rd] = result;
            cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                | (result & 0x80000000 ? FLAG_N : 0)
                | (result == 0 ? FLAG_Z : 0)
                | (carry_out ? FLAG_C : 0);
            // Thumb Format 1 always sets flags (no S bit -- implicit).
            break;
        }
case 0x1: { // Format 3: MOV/CMP/ADD/SUB immediate
            uint16_t op = (opcode >> 11) & 0x3; // bits [12:11]
            uint32_t rd = (opcode >> 8) & 0x7;
            uint32_t imm8 = opcode & 0xFF;
            uint32_t operand1 = cpu->r[rd];

            uint32_t result;
            bool carry = false, overflow = false;

            switch (op) {
                case 0x0: // MOV
                    result = imm8;
                    break;
                case 0x1: // CMP
                    result = operand1 - imm8;
                    carry = operand1 >= imm8;
                    overflow = ((operand1 ^ imm8) & (operand1 ^ result)) & 0x80000000;
                    break;
                case 0x2: // ADD
                    result = operand1 + imm8;
                    carry = result < operand1;
                    overflow = (~(operand1 ^ imm8) & (operand1 ^ result)) & 0x80000000;
                    break;
                case 0x3: // SUB
                    result = operand1 - imm8;
                    carry = operand1 >= imm8;
                    overflow = ((operand1 ^ imm8) & (operand1 ^ result)) & 0x80000000;
                    break;
                default:
                    result = operand1;
                    break;
            }

            // CMP discards the result (flags only); MOV/ADD/SUB write back.
            if (op != 0x1) {
                cpu->r[rd] = result;
            }

            uint32_t flag_mask = (op == 0x0) ? (FLAG_N | FLAG_Z) : (FLAG_N | FLAG_Z | FLAG_C | FLAG_V);
            cpu->cpsr = (cpu->cpsr & ~flag_mask)
                | (result & 0x80000000 ? FLAG_N : 0)
                | (result == 0 ? FLAG_Z : 0)
                | ((op != 0x0 && carry) ? FLAG_C : 0)
                | ((op != 0x0 && overflow) ? FLAG_V : 0);
            // MOV only sets N/Z (matches ARM MOV-immediate semantics with no shifter carry).
            break;
        }
case 0x2: {
            // Format 4/5/6/7/8 share top3==010 -- distinguished by bits [12:10]
            uint16_t sub = (opcode >> 10) & 0x7;

            if (sub == 0x0) {
                // Format 4: ALU operations
                uint16_t alu_op = (opcode >> 6) & 0xF;
                uint32_t rs = (opcode >> 3) & 0x7;
                uint32_t rd = opcode & 0x7;
                uint32_t operand1 = cpu->r[rd];
                uint32_t operand2 = cpu->r[rs];
                uint32_t result;
                bool write_back = true;
                bool carry = cpu->cpsr & FLAG_C, overflow = false;

                switch (alu_op) {
                    case 0x0: // AND
                        result = operand1 & operand2;
                        break;
                    case 0x1: // EOR
                        result = operand1 ^ operand2;
                        break;
                    case 0x2: { // LSL (by register, low byte of Rs)
                        uint32_t shift = operand2 & 0xFF;
                        if (shift == 0) {
                            result = operand1;
                        } else if (shift < 32) {
                            carry = (operand1 >> (32 - shift)) & 0x1;
                            result = operand1 << shift;
                        } else if (shift == 32) {
                            carry = operand1 & 0x1;
                            result = 0;
                        } else {
                            carry = false;
                            result = 0;
                        }
                        break;
                    }
                    case 0x3: { // LSR (by register, low byte of Rs)
                        uint32_t shift = operand2 & 0xFF;
                        if (shift == 0) {
                            result = operand1;
                        } else if (shift < 32) {
                            carry = (operand1 >> (shift - 1)) & 0x1;
                            result = operand1 >> shift;
                        } else if (shift == 32) {
                            carry = (operand1 >> 31) & 0x1;
                            result = 0;
                        } else {
                            carry = false;
                            result = 0;
                        }
                        break;
                    }
case 0x4: { // ASR (by register, low byte of Rs)
                        uint32_t shift = operand2 & 0xFF;
                        int32_t signed_val = (int32_t)operand1;
                        if (shift == 0) {
                            result = operand1;
                        } else if (shift < 32) {
                            carry = (operand1 >> (shift - 1)) & 0x1;
                            result = (uint32_t)(signed_val >> shift);
                        } else {
                            carry = (operand1 >> 31) & 0x1;
                            result = (uint32_t)(signed_val >> 31);
                        }
                        break;
                    }
                    case 0x5: { // ADC
                        uint32_t carry_in = (cpu->cpsr & FLAG_C) ? 1 : 0;
                        result = operand1 + operand2 + carry_in;
                        uint64_t wide = (uint64_t)operand1 + operand2 + carry_in;
                        carry = wide > 0xFFFFFFFFu;
                        overflow = (~(operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                        break;
                    }
                    case 0x6: { // SBC
                        uint32_t carry_in = (cpu->cpsr & FLAG_C) ? 1 : 0;
                        result = operand1 - operand2 - (1 - carry_in);
                        uint64_t wide = (uint64_t)operand1 - operand2 - (1 - carry_in);
                        carry = wide <= 0xFFFFFFFFu; // no borrow
                        overflow = ((operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                        break;
                    }
                    case 0x7: { // ROR (by register, low byte of Rs)
                        uint32_t shift = operand2 & 0xFF;
                        uint32_t effective = shift & 0x1F;
                        if (shift == 0) {
                            result = operand1;
                        } else if (effective == 0) {
                            carry = (operand1 >> 31) & 0x1;
                            result = operand1;
                        } else {
                            carry = (operand1 >> (effective - 1)) & 0x1;
                            result = (operand1 >> effective) | (operand1 << (32 - effective));
                        }
                        break;
                    }
case 0x8: // TST
                        result = operand1 & operand2;
                        write_back = false;
                        break;
                    case 0x9: // NEG
                        result = 0 - operand2;
                        carry = 0 >= operand2; // no borrow
                        overflow = ((0u ^ operand2) & (0u ^ result)) & 0x80000000;
                        break;
                    case 0xA: // CMP
                        result = operand1 - operand2;
                        carry = operand1 >= operand2;
                        overflow = ((operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                        write_back = false;
                        break;
                    case 0xB: // CMN
                        result = operand1 + operand2;
                        carry = result < operand1;
                        overflow = (~(operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                        write_back = false;
                        break;
                    case 0xC: // ORR
                        result = operand1 | operand2;
                        break;
                    case 0xD: // MUL
                        result = operand1 * operand2;
                        // TODO: real hardware leaves C flag as a meaningless
                        // "leftover" value from internal multiply cycles --
                        // approximating as unaffected (carry stays whatever
                        // it already was) rather than modeling the quirk.
                        break;
                    case 0xE: // BIC
                        result = operand1 & ~operand2;
                        break;
                    case 0xF: // MVN
                        result = ~operand2;
                        break;
                    default:
                        result = operand1;
                        write_back = false;
                        break;
                }

if (write_back) {
                    cpu->r[rd] = result;
                }
                bool affects_v = (alu_op == 0x5 || alu_op == 0x6 || alu_op == 0x9 || alu_op == 0xA || alu_op == 0xB); // ADC/SBC/NEG/CMP/CMN
                uint32_t flag_mask = affects_v ? (FLAG_N | FLAG_Z | FLAG_C | FLAG_V) : (FLAG_N | FLAG_Z | FLAG_C);
                cpu->cpsr = (cpu->cpsr & ~flag_mask)
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (carry ? FLAG_C : 0)
                    | ((affects_v && overflow) ? FLAG_V : 0);
                break;
            }

            if (sub == 0x1) {
                // Format 5: Hi Register Operations / BX
                uint16_t op = (opcode >> 8) & 0x3;
                bool h1 = (opcode >> 7) & 0x1;
                bool h2 = (opcode >> 6) & 0x1;
                uint32_t rs = ((opcode >> 3) & 0x7) | (h2 ? 0x8 : 0x0);
                uint32_t rd = (opcode & 0x7) | (h1 ? 0x8 : 0x0);

                uint32_t operand1 = cpu->r[rd];
                uint32_t operand2 = cpu->r[rs];
                // TODO: if rs or rd is R15, real hardware reads it as
                // (current instr addr + 4) due to pipelining -- cpu->r[15]
                // here already reflects the post-fetch +2, not +4. Same
                // class of quirk as the ARM PC-as-operand TODOs; revisit
                // once the pipeline/prefetch model is in place.

                switch (op) {
                    case 0x0: // ADD -- no flags set
                        cpu->r[rd] = operand1 + operand2;
                        break;
                    case 0x1: { // CMP -- flags only, full N/Z/C/V like ARM CMP
                        uint32_t result = operand1 - operand2;
                        bool carry = operand1 >= operand2; // no borrow
                        bool overflow = ((operand1 ^ operand2) & (operand1 ^ result)) & 0x80000000;
                        cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                            | (result & 0x80000000 ? FLAG_N : 0)
                            | (result == 0 ? FLAG_Z : 0)
                            | (carry ? FLAG_C : 0)
                            | (overflow ? FLAG_V : 0);
                        break;
                    }
                    case 0x2: // MOV -- no flags set
                        cpu->r[rd] = operand2;
                        break;
                    case 0x3: { // BX -- branch/exchange, h1 ignored per spec
                        bool switch_to_arm = !(operand2 & 0x1);
                        uint32_t target = operand2 & ~0x1u;
                        cpu->thumb_mode = !switch_to_arm;
                        cpu->r[15] = switch_to_arm ? (target & ~0x3u) : target;
                        break;
                    }
                    default:
                        break;
                }
                // Format 5 never sets flags except CMP (op == 0x1), handled above.
                break;
            }

            // TODO: Formats 6-8 (PC-relative load, load/store with register
            // offset, sign-extend load/store)
            break;
        }
        default:
            // stub - unimplemented group
            break;
    }
}
