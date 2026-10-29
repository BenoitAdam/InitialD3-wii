// NAOMI game boot header ("RomBootID", 0x500 bytes at offset 0 of the
// decrypted image). The BIOS uses it to copy the program into SH-4 RAM and
// jump to the entry point; it also copies it verbatim to RAM 0x0C01F400.
// Layout from Flycast's naomi_cart.h.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace idas3 {

constexpr uint32_t kNaomiBootHeaderSize = 0x500;
constexpr uint32_t kNaomiBootHeaderRamAddress = 0x0C01F400;

struct NaomiLoadEntry
{
    uint32_t offset = 0;    // offset in the DIMM image
    uint32_t address = 0;   // SH-4 destination address
    uint32_t size = 0;
};

struct NaomiBootHeader
{
    std::string boardName;      // "NAOMI" / "Naomi2"
    std::string vendorName;
    std::string titles[8];      // 0 Japan, 1 USA, 2 Export, 3 Korea, 4 Australia, 5-7 reserved
    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    std::string gameId;         // 4 chars
    uint16_t romMode = 0;
    std::vector<NaomiLoadEntry> gameLoad;
    std::vector<NaomiLoadEntry> testLoad;
    uint32_t gamePC = 0;
    uint32_t testPC = 0;
    uint8_t country = 0, cabinet = 0, resolution = 0, vertical = 0, serialId = 0, serviceMode = 0;

    bool isNaomi2() const;
    // Best title for display: Export, then Japan, then the first non-empty one.
    std::string displayTitle() const;
};

// Returns false (with a human readable reason) when the bytes are not a valid
// NAOMI header - for a GD-ROM image that almost always means a wrong PIC/key.
bool parseNaomiBootHeader(const uint8_t* data, NaomiBootHeader& out, std::string& error);

} // namespace idas3
