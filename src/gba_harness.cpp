// gba-core headless test harness
//
// Purpose: standalone CLI tool (no browser, no Hydra) to sanity-check the
// core in isolation. Loads a BIOS + ROM, runs the CPU, and reports state
// so we can see *where* things break instead of guessing.
//
// Usage (once implemented): gba_harness <bios.bin> <rom.gba> [--frames N]
//
// Plan:
//  - Load BIOS + ROM into GbaCoreState via gba_core_load_bios/load_rom
//  - Run N frames (or until a stuck-detection trips) via gba_core_run_frame
//  - Stuck detection: track PC history over the last few thousand steps --
//    if PC loops in a tiny range with no forward progress (common failure:
//    stuck in a BIOS wait loop or an unimplemented-opcode infinite loop),
//    flag it and dump state instead of running forever
//  - On any unimplemented opcode hit (our interpreter should assert/log
//    rather than silently no-op), print PC, opcode bytes, mode (ARM/Thumb),
//    and register dump, then halt
//  - Optional: dump framebuffer to a .ppm/.bmp every N frames for visual
//    sanity check without needing the full frontend
//  - Optional: step-by-step trace mode (--trace) that logs every
//    instruction executed, for diffing against a known-good trace log
//    from another emulator if we need to bisect a divergence

#include <cstdio>
#include "gba_core.h"

// TODO: void print_cpu_state(const GbaCpuState* cpu)
// TODO: bool detect_stuck(const uint32_t* pc_history, size_t history_len)
// TODO: void dump_framebuffer_ppm(const uint32_t* fb, int w, int h, const char* path)
// TODO: int main(int argc, char** argv)
//   - parse args (bios path, rom path, --frames, --trace)
//   - gba_core_init, load_bios, load_rom
//   - loop: gba_core_run_frame, check stuck detection, optional trace/dump
//   - on halt/stuck/unimplemented-opcode: print diagnostic, exit non-zero
//   - on clean completion of N frames: exit 0
