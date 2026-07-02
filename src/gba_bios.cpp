#include "gba_bios.h"

// BIOS implementation
//
// DELIVERY NOTE: this file only cares about receiving a byte buffer --
// how those bytes arrive is a frontend concern, not a core concern:
//  - Hydra (native): read file from configured path via fread(), pass
//    bytes+size straight to gba_bios_load()
//  - Browser (WASM): no filesystem access. Frontend JS gets bytes via
//    file upload (<input type=file>) or IndexedDB cache (avoid re-upload
//    every session), then copies them into WASM memory and calls
//    gba_bios_load() same as native -- same pattern as ROM loading.
// Core stays portable either way; keep it that way.
//
// Plan:
//  - gba_bios_load: copy/reference the given buffer, run validation
//  - gba_bios_validate: check size == 16384 bytes (real GBA BIOS size);
//    optionally checksum against known-good BIOS hash later

// TODO: bool gba_bios_load(GbaBiosDescriptor* bios, const uint8_t* data, size_t size)
// TODO: bool gba_bios_validate(const GbaBiosDescriptor* bios)
