#include "gba_bios.h"
#include <cstring>

// BIOS implementation
//
// See gba_bios.h for the delivery-path plan (native fread vs browser
// upload/IndexedDB) -- this file only touches bytes it's handed.

bool gba_bios_load(GbaBiosDescriptor* bios, const uint8_t* data, size_t size) {
    if (bios == nullptr || data == nullptr || size == 0) {
        return false;
    }

    // Caller-owned, not copied -- matches GbaMemory::rom convention
    // (see gba_bios.h struct comment).
    bios->data = data;
    bios->size = size;
    bios->is_valid = gba_bios_validate(bios);

    return bios->is_valid;
}

bool gba_bios_validate(const GbaBiosDescriptor* bios) {
    if (bios == nullptr || bios->data == nullptr) {
        return false;
    }

    // Real GBA BIOS is exactly 16 KB. Not checksumming against a known
    // hash yet (see header TODO) -- deliberately permissive so homebrew
    ///open-source replacement BIOSes of the correct size aren't rejected.
    if (bios->size != 0x4000) {
        return false;
    }

    return true;
}