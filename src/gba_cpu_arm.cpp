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

// Fetches the 32-bit opcode at the current PC. Does NOT advance PC --
// caller (gba_cpu_step_arm) owns PC advancement since it needs to happen
// before the instruction executes (branches overwrite PC mid-instruction).
uint32_t gba_cpu_fetch_arm(GbaCpuState* cpu, GbaMemory* mem) {
    return gba_mem_read32(mem, cpu->r[15]);
}

// CPSR condition flag bit positions
static const uint32_t FLAG_N = 1u << 31;
static const uint32_t FLAG_Z = 1u << 30;
static const uint32_t FLAG_C = 1u << 29;
static const uint32_t FLAG_V = 1u << 28;

// Result of decoding a Data Processing operand2: the shifted value plus
// the carry-out of the shifter (only meaningful/used when S=1 on logical ops).
struct ShifterResult {
    uint32_t value;
    bool carry_out;
};

// Decodes operand2 for the REGISTER form (opcode bit 25 == 0) with an
// immediate shift amount (opcode bit 4 == 0). Register-specified shift
// amount (bit 4 == 1) and the immediate form (bit 25 == 1) are handled
// by separate helpers, added next.
ShifterResult gba_cpu_decode_operand2_reg_imm(GbaCpuState* cpu, uint32_t opcode) {
    uint32_t rm          = opcode & 0xF;
    uint32_t shift_type  = (opcode >> 5) & 0x3;  // 0=LSL 1=LSR 2=ASR 3=ROR
    uint32_t shift_amt   = (opcode >> 7) & 0x1F;
    uint32_t value       = cpu->r[rm];
    bool carry_in        = cpu->cpsr & FLAG_C; // fallback when shift_amt==0 for LSL

    switch (shift_type) {
        case 0: { // LSL
            if (shift_amt == 0) {
                return { value, carry_in };
            }
            bool carry_out = (value >> (32 - shift_amt)) & 0x1;
            return { value << shift_amt, carry_out };
        }
        case 1: { // LSR
            if (shift_amt == 0) {
                // LSR #0 means LSR #32
                bool carry_out = (value >> 31) & 0x1;
                return { 0, carry_out };
            }
            bool carry_out = (value >> (shift_amt - 1)) & 0x1;
            return { value >> shift_amt, carry_out };
        }
        case 2: { // ASR
            int32_t signed_value = (int32_t)value;
            if (shift_amt == 0) {
                // ASR #0 means ASR #32
                bool carry_out = (value >> 31) & 0x1;
                return { (uint32_t)(signed_value >> 31), carry_out };
            }
            bool carry_out = (value >> (shift_amt - 1)) & 0x1;
            return { (uint32_t)(signed_value >> shift_amt), carry_out };
        }
        case 3: { // ROR
            if (shift_amt == 0) {
                // ROR #0 means RRX: rotate right through carry by 1
                uint32_t result = (carry_in ? 0x80000000 : 0) | (value >> 1);
                bool carry_out = value & 0x1;
                return { result, carry_out };
            }
            uint32_t result = (value >> shift_amt) | (value << (32 - shift_amt));
            bool carry_out = (value >> (shift_amt - 1)) & 0x1;
            return { result, carry_out };
        }
        default:
            return { value, carry_in };
    }
}

// Decodes operand2 for the REGISTER form (opcode bit 25 == 0) with a
// register-specified shift amount (opcode bit 4 == 1). Shift amount is
// the bottom byte of Rs. Unlike the immediate-shift form, shift_amt == 0
// here is a genuine no-op for LSL/LSR/ASR (not "shift by 32") and ROR #0
// is a genuine no-op too (not RRX) -- RRX only exists in the immediate
// encoding.
ShifterResult gba_cpu_decode_operand2_reg_reg(GbaCpuState* cpu, uint32_t opcode) {
    uint32_t rm         = opcode & 0xF;
    uint32_t shift_type = (opcode >> 5) & 0x3;
    uint32_t rs         = (opcode >> 8) & 0xF;
    uint32_t value       = cpu->r[rm];
    uint32_t shift_amt   = cpu->r[rs] & 0xFF;
    bool carry_in        = cpu->cpsr & FLAG_C;

    // TODO: if rm or rs == 15, PC reads as PC+12 (not +8) in this form --
    // not yet handled, same deferred PC-as-operand quirk noted elsewhere.

    if (shift_amt == 0) {
        return { value, carry_in }; // true no-op, flags unaffected
    }

    switch (shift_type) {
        case 0: { // LSL
            if (shift_amt >= 32) {
                bool carry_out = (shift_amt == 32) ? (value & 0x1) : false;
                return { 0, carry_out };
            }
            bool carry_out = (value >> (32 - shift_amt)) & 0x1;
            return { value << shift_amt, carry_out };
        }
        case 1: { // LSR
            if (shift_amt >= 32) {
                bool carry_out = (shift_amt == 32) ? ((value >> 31) & 0x1) : false;
                return { 0, carry_out };
            }
            bool carry_out = (value >> (shift_amt - 1)) & 0x1;
            return { value >> shift_amt, carry_out };
        }
        case 2: { // ASR
            int32_t signed_value = (int32_t)value;
            if (shift_amt >= 32) {
                bool carry_out = (value >> 31) & 0x1;
                return { (uint32_t)(signed_value >> 31), carry_out };
            }
            bool carry_out = (value >> (shift_amt - 1)) & 0x1;
            return { (uint32_t)(signed_value >> shift_amt), carry_out };
        }
        case 3: { // ROR
            uint32_t effective = shift_amt & 0x1F;
            if (effective == 0) {
                // full multiple of 32: value unchanged, carry = bit 31
                bool carry_out = (value >> 31) & 0x1;
                return { value, carry_out };
            }
            uint32_t result = (value >> effective) | (value << (32 - effective));
            bool carry_out = (value >> (effective - 1)) & 0x1;
            return { result, carry_out };
        }
        default:
            return { value, carry_in };
    }
}

// Decodes operand2 for the IMMEDIATE form (opcode bit 25 == 1): an 8-bit
// constant rotated right by (4-bit rotate field * 2).
ShifterResult gba_cpu_decode_operand2_imm(GbaCpuState* cpu, uint32_t opcode) {
    uint32_t imm8   = opcode & 0xFF;
    uint32_t rotate = ((opcode >> 8) & 0xF) * 2;
    bool carry_in   = cpu->cpsr & FLAG_C;

    if (rotate == 0) {
        return { imm8, carry_in }; // no rotation, carry unaffected
    }

    uint32_t result = (imm8 >> rotate) | (imm8 << (32 - rotate));
    bool carry_out = (result >> 31) & 0x1;
    return { result, carry_out };
}

// Cycle model: coarse S/N-cycle approximation, not wait-state-accurate.
// Base cost is 1S (register-only instruction). Each additional memory
// access adds 1N; writing PC (branch/pipeline refill) adds 2S. Good
// enough to drive PPU/timer/DMA advancement for now -- see gba_cpu.h note.
uint32_t gba_cpu_step_arm(GbaCpuState* cpu, GbaMemory* mem) {
    uint32_t opcode = gba_cpu_fetch_arm(cpu, mem);
    uint32_t cycles = 1;

    // ARM PC always points 8 bytes ahead of the executing instruction
    // (2-stage pipeline). Advance by 4 here for the *next* fetch; the
    // full +8 read-side offset is handled wherever r[15] is read as an
    // operand (not yet implemented).
    cpu->r[15] += 4;

    uint32_t cond_bits = opcode >> 28;
    if (!gba_cpu_check_condition(cpu->cpsr, cond_bits)) {
        return cycles; // condition failed, instruction is a no-op, still costs 1S
    }

// PSR Transfer group: bits [27:26] == 00, bits [24:23] == 10, bit 20 == 0
    if (((opcode >> 26) & 0x3) == 0x0 && ((opcode >> 23) & 0x3) == 0x2 && !((opcode >> 20) & 0x1)) {
        bool use_spsr = (opcode >> 22) & 0x1;
        bool is_msr   = (opcode >> 21) & 0x1;

// Maps current privileged mode to its SPSR bank index. User and
        // System modes have no SPSR -- accessing it there is UNPREDICTABLE
        // per the ARM spec; we fall back to CPSR in that case.
        int spsr_bank = -1;
        switch (cpu->current_mode) {
            case GBA_MODE_FIQ:        spsr_bank = GBA_BANK_FIQ; break;
            case GBA_MODE_SUPERVISOR: spsr_bank = GBA_BANK_SVC; break;
            case GBA_MODE_ABORT:      spsr_bank = GBA_BANK_ABT; break;
            case GBA_MODE_IRQ:        spsr_bank = GBA_BANK_IRQ; break;
            case GBA_MODE_UNDEFINED:  spsr_bank = GBA_BANK_UND; break;
            default: break; // USER / SYSTEM: no SPSR
        }

        if (!is_msr) { // MRS: transfer PSR to register
            uint32_t rd = (opcode >> 12) & 0xF;
            if (use_spsr && spsr_bank >= 0) {
                cpu->r[rd] = cpu->spsr[spsr_bank];
            } else {
                cpu->r[rd] = cpu->cpsr; // CPSR, or SPSR-in-User/System fallback
            }
        } else { // MSR: transfer register/immediate to PSR
            bool immediate = (opcode >> 25) & 0x1;
            uint32_t value;
            if (immediate) {
                uint32_t imm8   = opcode & 0xFF;
                uint32_t rotate = ((opcode >> 8) & 0xF) * 2;
                value = (rotate == 0) ? imm8 : ((imm8 >> rotate) | (imm8 << (32 - rotate)));
            } else {
                uint32_t rm = opcode & 0xF;
                value = cpu->r[rm];
            }

            // Field mask bits, opcode [19:16]: f(19)=flags, s(18)=status,
            // x(17)=extension, c(16)=control. GBA/ARMv4T only really uses
            // f (top byte: N/Z/C/V) and c (bottom byte: mode/I/F/T) -- s/x
            // bytes are reserved on this architecture, so only f and c are
            // handled here.
            bool write_flags   = (opcode >> 19) & 0x1;
            bool write_control = (opcode >> 16) & 0x1;

            uint32_t mask = 0;
            if (write_flags)   mask |= 0xFF000000;
            if (write_control) mask |= 0x000000FF;

            if (use_spsr && spsr_bank >= 0) {
                cpu->spsr[spsr_bank] = (cpu->spsr[spsr_bank] & ~mask) | (value & mask);
} else if (!use_spsr) {
                if (write_control) {
                    // Mode bits (0-4) go through gba_cpu_switch_mode to keep
                    // r[]/banking consistent. I/F/T bits (5-7) are not part
                    // of mode banking -- write them directly.
                    uint32_t new_mode_bits = value & 0x1F;
                    GbaCpuMode new_mode = (GbaCpuMode)new_mode_bits;

                    // TODO: no validation that new_mode_bits is one of the
                    // 6 legal GbaCpuMode values -- an illegal mode value in
                    // the written data will misbehave. Add a switch/default
                    // check here once we hit real-ROM cases that need it.
                    gba_cpu_switch_mode(cpu, new_mode);

uint32_t itf_mask = mask & 0xE0; // bits 5-7 (T, F, I) within the control byte
                    cpu->cpsr = (cpu->cpsr & ~itf_mask) | (value & itf_mask);
                    cpu->thumb_mode = (cpu->cpsr & 0x20) != 0; // keep cached flag in sync
                }
                if (write_flags) {
                    cpu->cpsr = (cpu->cpsr & ~0xFF000000) | (value & 0xFF000000);
                }
            }
        }
        return cycles; // MRS/MSR: register-only, 1S
    }

// Branch and Exchange group: bits [27:4] == 0001 0010 1111 1111 1111 0001
    if (((opcode >> 4) & 0xFFFFFF) == 0x12FFF1) {
        uint32_t rm = opcode & 0xF;
        uint32_t target = cpu->r[rm];

bool switch_to_thumb = target & 0x1;
        cpu->thumb_mode = switch_to_thumb;
        cpu->cpsr = switch_to_thumb ? (cpu->cpsr | 0x20) : (cpu->cpsr & ~0x20u);

        cpu->r[15] = target & ~0x1; // clear bit 0, it's not part of the address
        return cycles + 2; // BX writes PC: pipeline refill, +2S
    }

// Software Interrupt group: bits [27:24] == 1111
    if (((opcode >> 24) & 0xF) == 0xF) {
        // TODO: comment number (opcode & 0xFFFFFF) not used by hardware --
        // BIOS reads it from the instruction itself if needed. Not decoded here.
        gba_cpu_enter_exception(cpu, GBA_MODE_SUPERVISOR, 0x08);
        return cycles + 2; // SWI: exception entry writes PC, +2S
    }

// Branch / Branch-Link group: bits [27:25] == 101
    if (((opcode >> 25) & 0x7) == 0x5) {
        bool link = (opcode >> 24) & 0x1; // L bit

        // 24-bit signed offset, shifted left 2 (word-aligned), sign-extended.
        int32_t offset = opcode & 0xFFFFFF;
        if (offset & 0x800000) {
            offset |= 0xFF000000; // sign-extend
        }
        offset <<= 2;

        if (link) {
            // LR gets the return address: PC has already been advanced
            // +4 past this instruction at the top of gba_cpu_step_arm,
            // so r[15] currently holds (instruction_addr + 4), which is
            // exactly the correct return address.
            cpu->r[14] = cpu->r[15];
        }

        // Branch target is relative to PC+8 (the ARM pipeline's read-side
        // offset), not PC+4. r[15] here is only +4 past the instruction,
        // so add another +4 to match the +8 rule used for PC-relative math.
        cpu->r[15] = cpu->r[15] + 4 + offset;
        return cycles + 2; // B/BL writes PC: pipeline refill, +2S
    }

// Block Data Transfer group: bits [27:25] == 100 (LDM/STM)
    if (((opcode >> 25) & 0x7) == 0x4) {
        bool pre_index   = (opcode >> 24) & 0x1; // P bit
        bool add_offset  = (opcode >> 23) & 0x1; // U bit
        bool psr_or_user = (opcode >> 22) & 0x1; // S bit
        bool writeback   = (opcode >> 21) & 0x1; // W bit
        bool is_load     = (opcode >> 20) & 0x1; // L bit
        uint32_t rn      = (opcode >> 16) & 0xF;
        uint16_t reg_list = opcode & 0xFFFF;

uint32_t base = cpu->r[rn];
        int num_regs = 0;
        for (int i = 0; i < 16; i++) {
            if (reg_list & (1 << i)) num_regs++;
        }

        // Address stepping: each register transfer uses one word (4 bytes).
        // Direction (U bit) determines whether we walk up or down from base;
        // P bit determines pre- vs post-increment at each step.
        uint32_t addr = add_offset ? base : (base - num_regs * 4);
        if (!add_offset) {
            // Walking downward: lowest register in the list goes to the
            // lowest address, so we start at (base - total size) and walk
            // up from there. This matches ARM's documented behavior for
            // decrement addressing modes (equivalent to DA/DB in practice).
        }

        uint32_t final_base = add_offset ? (base + num_regs * 4) : (base - num_regs * 4);

        // TODO: PSR/user-bank register transfers (S bit) not yet handled --
        // affects LDM with PC in list (restores CPSR from SPSR) and
        // accessing user-mode registers from privileged modes.
        (void)psr_or_user;

for (int i = 0; i < 16; i++) {
            if (!(reg_list & (1 << i))) continue;

            if (pre_index) addr += 4;

            if (is_load) {
                cpu->r[i] = gba_mem_read32(mem, addr);
                // TODO: if i == 15 (PC), this is a branch -- pipeline
                // refill not yet handled here.
            } else {
                gba_mem_write32(mem, addr, cpu->r[i]);
                // TODO: storing PC (i==15) reads as PC+12 on some ARM
                // cores -- not yet handled here.
            }

            if (!pre_index) addr += 4;
        }

        if (writeback) {
            cpu->r[rn] = final_base;
        }

        cycles += num_regs; // 1N per register transferred
        if (is_load && (reg_list & (1 << 15))) {
            cycles += 2; // LDM loading PC: pipeline refill
        }
        return cycles;
    }

// Single Data Transfer group: bits [27:26] == 01 (LDR/STR)
    if (((opcode >> 26) & 0x3) == 0x1) {
        bool immediate_offset = !((opcode >> 25) & 0x1); // I bit: 0=imm, 1=register
        bool pre_index        = (opcode >> 24) & 0x1;    // P bit
        bool add_offset       = (opcode >> 23) & 0x1;    // U bit: 1=add, 0=subtract
        bool byte_transfer    = (opcode >> 22) & 0x1;    // B bit
        bool writeback        = (opcode >> 21) & 0x1;    // W bit
        bool is_load          = (opcode >> 20) & 0x1;    // L bit
        uint32_t rn           = (opcode >> 16) & 0xF;
        uint32_t rd           = (opcode >> 12) & 0xF;

        uint32_t offset;
        if (immediate_offset) {
            offset = opcode & 0xFFF; // 12-bit immediate
        } else {
            // Register offset, optionally shifted -- reuse the same
            // immediate-shift decode as Data Processing (bit 4 must be 0
            // here per spec; register-shift-by-register is not valid in
            // this addressing mode).
            offset = gba_cpu_decode_operand2_reg_imm(cpu, opcode).value;
        }

uint32_t base = cpu->r[rn];
        uint32_t offset_addr = add_offset ? (base + offset) : (base - offset);

        // Pre-indexed: use offset_addr as the transfer address, and if W
        // is set, write it back to Rn. Post-indexed: use base as the
        // transfer address, and ALWAYS write offset_addr back to Rn
        // regardless of W (W means something different in post-index --
        // it selects privileged/user-mode memory translation, which we're
        // not modeling -- so writeback always happens in post-index).
        uint32_t transfer_addr = pre_index ? offset_addr : base;
if (is_load) {
            if (byte_transfer) {
                cpu->r[rd] = gba_mem_read8(mem, transfer_addr);
            } else {
                // TODO: unaligned word loads should rotate the read value
                // per ARM's LDR rotation quirk -- not yet handled.
                cpu->r[rd] = gba_mem_read32(mem, transfer_addr);
            }
            // TODO: if rd == 15 (PC), this is a branch -- pipeline refill
            // not yet handled here.
} else {
            if (byte_transfer) {
                gba_mem_write8(mem, transfer_addr, (uint8_t)(cpu->r[rd] & 0xFF));
            } else {
                gba_mem_write32(mem, transfer_addr, cpu->r[rd]);
            }
            // TODO: storing PC (rd==15) reads as PC+12 on some ARM cores --
            // not yet handled here.
        }

        if (pre_index) {
            if (writeback) {
                cpu->r[rn] = offset_addr;
            }
        } else {
            cpu->r[rn] = offset_addr; // post-index always writes back
        }

        cycles += 1; // 1N for the single memory access
        if (is_load && rd == 15) {
            cycles += 2; // LDR loading PC: pipeline refill
        }
        return cycles;
    }

// Multiply group: bits [27:22] == 000000, bit 7 == 1, bit 4 == 1
    if (((opcode >> 22) & 0x3F) == 0x0 && ((opcode >> 7) & 0x1) && ((opcode >> 4) & 0x1)) {
        bool accumulate = (opcode >> 21) & 0x1;
        bool set_flags  = (opcode >> 20) & 0x1;
        uint32_t rd     = (opcode >> 16) & 0xF; // destination
        uint32_t rn     = (opcode >> 12) & 0xF; // accumulate operand (MLA only)
        uint32_t rs     = (opcode >> 8) & 0xF;
        uint32_t rm     = opcode & 0xF;

        uint32_t result = cpu->r[rm] * cpu->r[rs];
        if (accumulate) {
            result += cpu->r[rn]; // MLA
        }
        cpu->r[rd] = result;

        if (set_flags) {
            cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z))
                | (result & 0x80000000 ? FLAG_N : 0)
                | (result == 0 ? FLAG_Z : 0);
            // Multiply never affects C or V flags per ARM spec.
        }
        // TODO: Rd must not be R15, Rm must not equal Rd on some ARM
// TODO: Rd must not be R15, Rm must not equal Rd on some ARM
        // variants -- undefined behavior cases not yet validated.

        // Real hardware's multiply takes a variable number of internal
        // cycles depending on Rs's value (early-termination logic) --
        // approximating with a flat +2 for now (+3 if accumulating).
        cycles += accumulate ? 3 : 2;
        return cycles;
    }

// Multiply Long group: bits [27:23] == 00001, bit 7 == 1, bit 4 == 1
    if (((opcode >> 23) & 0x1F) == 0x1 && ((opcode >> 7) & 0x1) && ((opcode >> 4) & 0x1)) {
        bool is_signed  = (opcode >> 22) & 0x1;
        bool accumulate = (opcode >> 21) & 0x1;
        bool set_flags  = (opcode >> 20) & 0x1;
        uint32_t rdhi   = (opcode >> 16) & 0xF;
        uint32_t rdlo   = (opcode >> 12) & 0xF;
        uint32_t rs     = (opcode >> 8) & 0xF;
        uint32_t rm     = opcode & 0xF;

        uint64_t result;
        if (is_signed) {
            int64_t product = (int64_t)(int32_t)cpu->r[rm] * (int64_t)(int32_t)cpu->r[rs];
            result = (uint64_t)product;
        } else {
            result = (uint64_t)cpu->r[rm] * (uint64_t)cpu->r[rs];
        }

        if (accumulate) {
            uint64_t acc = ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
            result += acc; // wraps correctly for both signed/unsigned in 64-bit
        }

        cpu->r[rdlo] = (uint32_t)(result & 0xFFFFFFFF);
        cpu->r[rdhi] = (uint32_t)(result >> 32);

        if (set_flags) {
            cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z))
                | (cpu->r[rdhi] & 0x80000000 ? FLAG_N : 0)
                | (result == 0 ? FLAG_Z : 0);
            // Multiply Long never affects C or V flags per ARM spec.
        }
        // TODO: RdHi, RdLo, Rm must all be distinct and not R15 --
        // undefined behavior cases not yet validated.

        // Same approximation note as the 32-bit Multiply group above.
        cycles += accumulate ? 4 : 3;
        return cycles;
    }

// Data Processing group: bits [27:26] == 00
    if (((opcode >> 26) & 0x3) == 0x0) {
uint32_t dp_opcode = (opcode >> 21) & 0xF;
        bool set_flags     = (opcode >> 20) & 0x1;
        uint32_t rn        = (opcode >> 16) & 0xF;
        uint32_t rd        = (opcode >> 12) & 0xF;

        // Decode operand2 once per instruction based on bit 25 (I) and,
        // for register form, bit 4 (shift-by-register vs shift-by-immediate).
        ShifterResult op2;
        if ((opcode >> 25) & 0x1) {
            op2 = gba_cpu_decode_operand2_imm(cpu, opcode);
        } else if ((opcode >> 4) & 0x1) {
            op2 = gba_cpu_decode_operand2_reg_reg(cpu, opcode);
        } else {
            op2 = gba_cpu_decode_operand2_reg_imm(cpu, opcode);
        }
        uint32_t operand2 = op2.value;

        switch (dp_opcode) {
            case 0x0: { // AND
                uint32_t result = cpu->r[rn] & operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
case 0x1: { // EOR
                uint32_t result = cpu->r[rn] ^ operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
case 0x2: { // SUB
                uint32_t result = cpu->r[rn] - operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    bool carry = cpu->r[rn] >= operand2; // no borrow
                    bool overflow = ((cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x3: { // RSB
                uint32_t result = operand2 - cpu->r[rn];
                cpu->r[rd] = result;

                if (set_flags) {
                    bool carry = operand2 >= cpu->r[rn]; // no borrow
                    bool overflow = ((operand2 ^ cpu->r[rn]) & (operand2 ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x4: { // ADD
                uint32_t result = cpu->r[rn] + operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    bool carry = result < cpu->r[rn]; // unsigned overflow
                    bool overflow = (~(cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x5: { // ADC
                uint32_t carry_in = (cpu->cpsr & FLAG_C) ? 1 : 0;
                uint32_t result = cpu->r[rn] + operand2 + carry_in;
                cpu->r[rd] = result;

                if (set_flags) {
                    uint64_t wide = (uint64_t)cpu->r[rn] + operand2 + carry_in;
                    bool carry = wide > 0xFFFFFFFFu;
                    bool overflow = (~(cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x6: { // SBC
                uint32_t carry_in = (cpu->cpsr & FLAG_C) ? 1 : 0;
                uint32_t result = cpu->r[rn] - operand2 - (1 - carry_in);

                cpu->r[rd] = result;

                if (set_flags) {
                    uint64_t wide = (uint64_t)cpu->r[rn] - operand2 - (1 - carry_in);
                    bool carry = wide <= 0xFFFFFFFFu; // no borrow
                    bool overflow = ((cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x7: { // RSC
                uint32_t carry_in = (cpu->cpsr & FLAG_C) ? 1 : 0;
                uint32_t result = operand2 - cpu->r[rn] - (1 - carry_in);

                cpu->r[rd] = result;

                if (set_flags) {
                    uint64_t wide = (uint64_t)operand2 - cpu->r[rn] - (1 - carry_in);
                    bool carry = wide <= 0xFFFFFFFFu; // no borrow
                    bool overflow = ((operand2 ^ cpu->r[rn]) & (operand2 ^ result)) & 0x80000000;
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (carry ? FLAG_C : 0)
                        | (overflow ? FLAG_V : 0);
                }
                break;
            }
case 0x8: { // TST
                uint32_t result = cpu->r[rn] & operand2;
                // TST always sets flags (S bit implied), no writeback to rd.

                cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (op2.carry_out ? FLAG_C : 0);
                break;
            }
case 0x9: { // TEQ
                uint32_t result = cpu->r[rn] ^ operand2;
                // TEQ always sets flags, no writeback to rd.

                cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (op2.carry_out ? FLAG_C : 0);
                break;
            }
case 0xA: { // CMP
                uint32_t result = cpu->r[rn] - operand2;
                // CMP always sets flags, no writeback to rd.

                bool carry = cpu->r[rn] >= operand2; // no borrow
                bool overflow = ((cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (carry ? FLAG_C : 0)
                    | (overflow ? FLAG_V : 0);
                break;
            }
case 0xB: { // CMN
                uint32_t result = cpu->r[rn] + operand2;
                // CMN always sets flags, no writeback to rd.

                bool carry = result < cpu->r[rn]; // unsigned overflow
                bool overflow = (~(cpu->r[rn] ^ operand2) & (cpu->r[rn] ^ result)) & 0x80000000;
                cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V))
                    | (result & 0x80000000 ? FLAG_N : 0)
                    | (result == 0 ? FLAG_Z : 0)
                    | (carry ? FLAG_C : 0)
                    | (overflow ? FLAG_V : 0);
                break;
            }
case 0xC: { // ORR
                uint32_t result = cpu->r[rn] | operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
case 0xD: { // MOV
                // Note: MOV ignores rn entirely.
                uint32_t result = operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
case 0xE: { // BIC
                uint32_t result = cpu->r[rn] & ~operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
            case 0xF: { // MVN
                uint32_t result = ~operand2;
                cpu->r[rd] = result;

                if (set_flags) {
                    cpu->cpsr = (cpu->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C))
                        | (result & 0x80000000 ? FLAG_N : 0)
                        | (result == 0 ? FLAG_Z : 0)
                        | (op2.carry_out ? FLAG_C : 0);
                }
                break;
            }
            default:
                break;
        }

        // TST/TEQ/CMP/CMN (0x8-0xB) never write rd, so no PC-refill case.
        bool writes_rd = dp_opcode < 0x8 || dp_opcode > 0xB;
        if (writes_rd && rd == 15) {
            cycles += 2; // result written to PC: pipeline refill
        }
        return cycles;
    }

    // Unimplemented/reserved opcode -- no decode group matched above.
    // TODO: harness's unimplemented-opcode halt+dump hook goes here.
    return cycles;
}

// Evaluates the top-4-bit ARM condition code
// cond_bits is expected to already be isolated to bits [31:28] of the
// fetched opcode, shifted down to [3:0] (i.e. 0x0-0xF).
bool gba_cpu_check_condition(uint32_t cpsr, uint32_t cond_bits) {
    bool n = cpsr & FLAG_N;
    bool z = cpsr & FLAG_Z;
    bool c = cpsr & FLAG_C;
    bool v = cpsr & FLAG_V;

    switch (cond_bits) {
        case 0x0: return z;                    // EQ
        case 0x1: return !z;                   // NE
        case 0x2: return c;                    // CS/HS
        case 0x3: return !c;                   // CC/LO
        case 0x4: return n;                    // MI
        case 0x5: return !n;                   // PL
        case 0x6: return v;                    // VS
        case 0x7: return !v;                   // VC
        case 0x8: return c && !z;              // HI
        case 0x9: return !c || z;              // LS
        case 0xA: return n == v;               // GE
        case 0xB: return n != v;               // LT
        case 0xC: return !z && (n == v);       // GT
        case 0xD: return z || (n != v);        // LE
        case 0xE: return true;                 // AL
        case 0xF: return false;                // NV (reserved, never executes)
        default:  return false;
    }
}
