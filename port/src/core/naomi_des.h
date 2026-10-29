// DES as implemented by the NAOMI DIMM board (Sega 315-6322) for GD-ROM
// game images: plain ECB DES over 64-bit little-endian blocks.
//
// Derived from MAME's naomigd.cpp (license: BSD-3-Clause, copyright-holders:
// Olivier Galibert) via Flycast's gdcartridge.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace idas3 {

class NaomiDes
{
public:
    // `picKey` is the key exactly as read from the PIC.
    void setKey(uint64_t picKey);

    uint64_t decryptBlock(uint64_t block) const;
    uint64_t encryptBlock(uint64_t block) const;

    // Decrypts `bytes` (multiple of 8) in place. Each 8-byte block is read as
    // a little-endian u64, like the DIMM board's own bus, on every host.
    void decryptBuffer(uint8_t* data, size_t bytes) const;

private:
    uint32_t subkeys_[32] = {};
};

} // namespace idas3
