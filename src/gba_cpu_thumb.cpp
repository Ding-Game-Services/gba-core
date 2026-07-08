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

            if (sub == 0x2 || sub == 0x3) {
                // Format 6: PC-relative load -- LDR Rd, [PC, #Imm]
                // Encoding: 01001 [Rd:3][Word8:8]; sub only narrows this to
                // 010/011 because Rd's top bit leaks into the sub field --
                // Rd is re-extracted below from its real position (bits 10-8).
                uint32_t rd = (opcode >> 8) & 0x7;
                uint32_t word8 = opcode & 0xFF;

                // Spec: PC here means (instruction address + 4), word-aligned
                // (bit 1 forced to 0). cpu->r[15] was already advanced by 2
                // at fetch time, so +2 more recovers the spec's "PC".
                uint32_t base = (cpu->r[15] + 2) & ~0x3u;
                uint32_t address = base + (word8 << 2);

                cpu->r[rd] = gba_memory_read32(mem, address);
                // Format 6 sets no flags.
                break;
            }

if (sub == 0x4 || sub == 0x5 || sub == 0x6 || sub == 0x7) {
                // Format 7: Load/store with register offset (bit9 == 0)
                // Format 8: Sign-extended load/store (bit9 == 1)
                // Encoding: 0101 [Op:2][bit9][0][Ro:3][Rb:3][Rd:3]
                bool bit9 = (opcode >> 9) & 0x1;
                bool bit11 = (opcode >> 11) & 0x1; // L or S depending on format
                bool bit10 = (opcode >> 10) & 0x1; // B or H depending on format
                uint32_t ro = (opcode >> 6) & 0x7;
                uint32_t rb = (opcode >> 3) & 0x7;
                uint32_t rd = opcode & 0x7;
                uint32_t address = cpu->r[rb] + cpu->r[ro];

                if (!bit9) {
                    // Format 7: STR/STRB/LDR/LDRB, bit11=L, bit10=B
                    bool load = bit11;
                    bool byte = bit10;
                    if (load) {
                        cpu->r[rd] = byte ? gba_memory_read8(mem, address)
                                           : gba_memory_read32(mem, address);
                    } else {
                        if (byte) {
                            gba_memory_write8(mem, address, cpu->r[rd] & 0xFF);
                        } else {
                            gba_memory_write32(mem, address, cpu->r[rd]);
                        }
                    }
                } else {
                    // Format 8: STRH/LDRH/LDSB/LDSH, bit11=H, bit10=S
                    bool sign = bit10;
                    bool halfword = bit11;
                    if (!sign) {
                        if (halfword) {
                            // LDRH
                            cpu->r[rd] = gba_memory_read16(mem, address);
                        } else {
                            // STRH
                            gba_memory_write16(mem, address, cpu->r[rd] & 0xFFFF);
                        }
                    } else {
                        if (halfword) {
                            // LDSH -- sign-extend 16-bit
                            int16_t val = (int16_t)gba_memory_read16(mem, address);
                            cpu->r[rd] = (uint32_t)(int32_t)val;
                        } else {
                            // LDSB -- sign-extend 8-bit
                            int8_t val = (int8_t)gba_memory_read8(mem, address);
                            cpu->r[rd] = (uint32_t)(int32_t)val;
                        }
                    }
                }
                // Formats 7 & 8 set no flags.
                break;
            }
            break;
        }
case 0x3: {
            // Format 9: Load/store with immediate offset
            // Encoding: 011 [B][L][Offset5:5][Rb:3][Rd:3]
            bool byte = (opcode >> 12) & 0x1;
            bool load = (opcode >> 11) & 0x1;
            uint32_t offset5 = (opcode >> 6) & 0x1F;
            uint32_t rb = (opcode >> 3) & 0x7;
            uint32_t rd = opcode & 0x7;

            // Offset5 is a word count for word transfers, byte count for
            // byte transfers -- spec scales it by 4 only when !byte.
            uint32_t offset = byte ? offset5 : (offset5 << 2);
            uint32_t address = cpu->r[rb] + offset;

            if (load) {
                cpu->r[rd] = byte ? gba_memory_read8(mem, address)
                                   : gba_memory_read32(mem, address);
            } else {
                if (byte) {
                    gba_memory_write8(mem, address, cpu->r[rd] & 0xFF);
                } else {
                    gba_memory_write32(mem, address, cpu->r[rd]);
                }
            }
            // Format 9 sets no flags.
            break;
        }
case 0x4: {
            bool is_format11 = (opcode >> 12) & 0x1;

            if (!is_format11) {
                // Format 10: Load/store halfword
                // Encoding: 1000 [L][Offset5:5][Rb:3][Rd:3]
                bool load = (opcode >> 11) & 0x1;
                uint32_t offset5 = (opcode >> 6) & 0x1F;
                uint32_t rb = (opcode >> 3) & 0x7;
                uint32_t rd = opcode & 0x7;
                uint32_t address = cpu->r[rb] + (offset5 << 1); // scaled by 2

                if (load) {
                    cpu->r[rd] = gba_memory_read16(mem, address);
                } else {
                    gba_memory_write16(mem, address, cpu->r[rd] & 0xFFFF);
                }
            } else {
                // Format 11: SP-relative load/store
                // Encoding: 1001 [L][Rd:3][Word8:8]
                bool load = (opcode >> 11) & 0x1;
                uint32_t rd = (opcode >> 8) & 0x7;
                uint32_t word8 = opcode & 0xFF;
                uint32_t address = cpu->r[13] + (word8 << 2); // SP is r13

                if (load) {
                    cpu->r[rd] = gba_memory_read32(mem, address);
                } else {
                    gba_memory_write32(mem, address, cpu->r[rd]);
                }
            }
            // Formats 10 & 11 set no flags.
            break;
        }
case 0x5: {
bool is_format13_or_14 = (opcode >> 12) & 0x1;
            bool is_format14 = is_format13_or_14 && ((opcode >> 10) & 0x1);

            if (!is_format13_or_14) {
                // Format 12: Load address -- ADD Rd, (PC|SP), #Imm
                // Encoding: 1010 [SP][Rd:3][Word8:8]
                bool use_sp = (opcode >> 11) & 0x1;
                uint32_t rd = (opcode >> 8) & 0x7;
                uint32_t word8 = opcode & 0xFF;

                uint32_t base;
                if (use_sp) {
                    base = cpu->r[13]; // SP
                } else {
                    // Spec: PC here means (instruction address + 4), word-aligned.
                    // cpu->r[15] was already advanced by 2 at fetch time.
                    base = (cpu->r[15] + 2) & ~0x3u;
                }
                cpu->r[rd] = base + (word8 << 2);
                // Format 12 sets no flags.
            } else {
if (!is_format14) {
                    // Format 13: Add offset to SP -- ADD SP, #+/-Imm
                    // Encoding: 1011 0000 [S][SWord7:7]
                    bool negative = (opcode >> 7) & 0x1;
                    uint32_t sword7 = opcode & 0x7F;
                    uint32_t offset = sword7 << 2;

                    cpu->r[13] = negative ? (cpu->r[13] - offset) : (cpu->r[13] + offset);
                    // Format 13 sets no flags.
                } else {
                    // Format 14: PUSH/POP registers
                    // Encoding: 1011 [L]10[R][Rlist:8]
                    bool pop = (opcode >> 11) & 0x1;
                    bool include_lr_pc = (opcode >> 8) & 0x1; // R bit: LR on push, PC on pop
                    uint32_t rlist = opcode & 0xFF;

                    uint32_t count = include_lr_pc ? 1 : 0;
                    for (int i = 0; i < 8; i++) {
                        if (rlist & (1 << i)) count++;
                    }

                    if (pop) {
                        uint32_t addr = cpu->r[13];
                        for (int i = 0; i < 8; i++) {
                            if (rlist & (1 << i)) {
                                cpu->r[i] = gba_memory_read32(mem, addr);
                                addr += 4;
                            }
                        }
                        if (include_lr_pc) {
                            cpu->r[15] = gba_memory_read32(mem, addr);
                            addr += 4;
                            // TODO: on ARMv5T, POP {PC} also acts like BX
                            // (bit0 of the loaded value selects ARM/Thumb).
                            // GBA's ARM7TDMI is ARMv4T, so POP {PC} always
                            // stays in the current state -- confirm this
                            // matches whatever mode-switch behavior the ARM
                            // BX handler settled on before relying on it.
                        }
                        cpu->r[13] = addr;
                    } else {
                        uint32_t addr = cpu->r[13] - (count * 4);
                        cpu->r[13] = addr;
                        for (int i = 0; i < 8; i++) {
                            if (rlist & (1 << i)) {
                                gba_memory_write32(mem, addr, cpu->r[i]);
                                addr += 4;
                            }
                        }
                        if (include_lr_pc) {
                            gba_memory_write32(mem, addr, cpu->r[14]);
                        }
                    }
                    // Format 14 sets no flags.
                }
            }
            break;
        }
case 0x6: {
          bool is_branch_or_swi = (opcode >> 12) & 0x1;
          if (!is_branch_or_swi) {
            // Format 15: Multiple load/store -- STMIA/LDMIA Rb!, {Rlist}
            // Encoding: 1100 [L][Rb:3][Rlist:8]
            bool load = (opcode >> 11) & 0x1;
            uint32_t rb = (opcode >> 8) & 0x7;
            uint32_t rlist = opcode & 0xFF;

            uint32_t addr = cpu->r[rb];
            // TODO: empty Rlist is UNPREDICTABLE per spec -- not handled,
            // assuming well-formed ROMs for now (same class as other
            // deferred UNPREDICTABLE-case TODOs).
            if (load) {
                for (int i = 0; i < 8; i++) {
                    if (rlist & (1 << i)) {
                        cpu->r[i] = gba_memory_read32(mem, addr);
                        addr += 4;
                    }
                }
                // TODO: base register write-back when Rb is in Rlist for a
                // load is UNPREDICTABLE per spec -- current code always
                // writes back the final address regardless. Revisit if a
                // test ROM exercises this.
            } else {
                for (int i = 0; i < 8; i++) {
                    if (rlist & (1 << i)) {
                        gba_memory_write32(mem, addr, cpu->r[i]);
                        addr += 4;
                    }
                }
            }
cpu->r[rb] = addr;
            // Format 15 sets no flags.
          } else {
            uint32_t cond = (opcode >> 8) & 0xF;

            if (cond == 0xF) {
// Format 17: SWI -- ARM's SWI decode is also still a TODO
                // stub (see gba_cpu_arm.cpp, "remaining decode groups"
                // comment), so nothing to hook into yet. Wire both up
                // together when SWI/BIOS call dispatch gets built.
            } else if (cond == 0xE) {
                // Undefined instruction space per spec -- not handled.
            } else {
                // Format 16: Conditional branch
                // Encoding: 1101 [Cond:4][SOffset8:8]
                int8_t soffset8 = (int8_t)(opcode & 0xFF);
                int32_t offset = ((int32_t)soffset8) << 1;

                // ASSUMPTION: reusing the same condition-check helper the
                // ARM conditional-execution path uses. Flag me if that
                // function has a different name/signature than this.
                if (gba_cpu_check_condition(cpu->cpsr, cond)) {
                    // Spec: PC here means (instruction address + 4).
                    // cpu->r[15] was already advanced by 2 at fetch time.
                    uint32_t base = cpu->r[15] + 2;
                    cpu->r[15] = base + offset;
                }
            }
          }
          break;
        }
case 0x7: {
            bool is_long_branch_link = (opcode >> 12) & 0x1;

            if (!is_long_branch_link) {
                // Format 18: Unconditional branch
                // Encoding: 11100 [Offset11:11]
                uint32_t offset11 = opcode & 0x7FF;
                // Sign-extend 11-bit field, then scale by 2.
                int32_t signed_offset = (int32_t)(offset11 << 21) >> 21;
                int32_t offset = signed_offset << 1;

                // Spec: PC here means (instruction address + 4).
                // cpu->r[15] was already advanced by 2 at fetch time.
                uint32_t base = cpu->r[15] + 2;
                cpu->r[15] = base + offset;
                // Format 18 sets no flags.
} else {
                // Format 19: Long branch with link (two-instruction pair)
                // Encoding: 11110 [OffsetHigh:11] then 11111 [OffsetLow:11]
                bool second_half = (opcode >> 11) & 0x1;
                uint32_t offset11 = opcode & 0x7FF;

                if (!second_half) {
                    // First instruction (H=0): stash a PC-relative target
                    // into LR as scratch. Sign-extend the 11-bit high part
                    // across the full 23 bits it represents before shifting.
                    int32_t signed_high = (int32_t)(offset11 << 21) >> 21;
                    uint32_t base = cpu->r[15] + 2; // spec PC = instr addr + 4
                    cpu->r[14] = base + ((uint32_t)signed_high << 12);
                } else {
                    // Second instruction (H=1): combine with LR, set LR to
                    // return address (instruction after this one) with bit0
                    // set (Thumb return marker), then branch.
                    uint32_t return_addr = cpu->r[15]; // already past this opcode
                    uint32_t target = cpu->r[14] + (offset11 << 1);
                    cpu->r[15] = target;
                    cpu->r[14] = return_addr | 0x1;
                }
                // Format 19 sets no flags.
            }
            break;
        }
        default:
            // stub - unimplemented group
            break;
    }
}