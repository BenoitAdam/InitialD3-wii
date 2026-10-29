// Explicit little-endian accessors.
//
// Everything the game ships (GD-ROM ISO9660 fields, the PIC, the decrypted
// DIMM image, the NAOMI boot header, SH-4 memory) is little-endian. The Wii's
// Broadway CPU is big-endian, so a plain `*(u32*)p` reads the wrong value
// there while looking correct on Windows. Never dereference game data
// directly: always go through these helpers.
#pragma once

#include <cstdint>

namespace idas3 {

inline uint16_t rd_le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

inline uint32_t rd_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint64_t rd_le64(const uint8_t* p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

inline void wr_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline void wr_le64(uint8_t* p, uint64_t v)
{
    wr_le32(p, (uint32_t)v);
    wr_le32(p + 4, (uint32_t)(v >> 32));
}

} // namespace idas3
