#ifndef GBA_CPU_H
#define GBA_CPU_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ARM7TDMI CPU modes (bottom 5 bits of CPSR)
typedef enum {
    GBA_MODE_USER       = 0x10,
    GBA_MODE_FIQ        = 0x11,
    GBA_MODE_IRQ        = 0x12,
    GBA_MODE_SUPERVISOR = 0x13,
    GBA_MODE_ABORT      = 0x17,
    GBA_MODE_UNDEFINED  = 0x1B,
    GBA_MODE_SYSTEM     = 0x1F
} GbaCpuMode;

// Index into r13_14_banked[] for each privileged mode's SP/LR pair
typedef enum {
    GBA_BANK_FIQ = 0,
    GBA_BANK_SVC = 1,
    GBA_BANK_ABT = 2,
    GBA_BANK_IRQ = 3,
    GBA_BANK_UND = 4,
    GBA_BANK_COUNT = 5
} GbaBankIndex;

typedef struct {
    uint32_t r[16];                    // active register view (R0-R15)
    uint32_t r8_12_fiq[5];             // banked FIQ-only regs (R8-R12)
    uint32_t r13_14_banked[GBA_BANK_COUNT][2]; // banked SP(0)/LR(1) per mode
    uint32_t cpsr;                     // current program status register
    uint32_t spsr[GBA_BANK_COUNT];     // saved CPSR per privileged mode
    bool thumb_mode;                   // cached T-bit, mirrors cpsr bit 5
    GbaCpuMode current_mode;           // cached mode bits, mirrors cpsr bits 0-4
} GbaCpuState;

// TODO: gba_cpu_init(GbaCpuState* cpu)
// TODO: gba_cpu_reset(GbaCpuState* cpu)
// TODO: gba_cpu_step(GbaCpuState* cpu, GbaMemory* mem)   single instruction step (ARM or Thumb dispatch)
// TODO: gba_cpu_switch_mode(GbaCpuState* cpu, GbaCpuMode new_mode) handles register banking on mode change
// TODO: gba_cpu_enter_exception(GbaCpuState* cpu, ...)   IRQ/SWI/etc entry sequence

#ifdef __cplusplus
}
#endif

#endif // GBA_CPU_H
