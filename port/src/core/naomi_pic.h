// NAOMI DIMM-board security PIC (e.g. 317-0384-com.pic for GDS-0033).
//
// The PIC16 on the DIMM board holds the 64-bit DES key used to decrypt the
// game image and the name of the encrypted file on the GD-ROM. Both a full
// 16 KB PIC dump and the older "extracted key" format are accepted, exactly
// as in MAME/Flycast.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace idas3 {

struct NaomiPic
{
    std::string fileName;      // file to look for in the GD-ROM root ("*.BIN" style allowed)
    uint64_t desKey = 0;       // key as stored in the PIC (before the rev64 MAME applies)
    bool netPic = false;       // net-boot PIC: game lives under a ROM/ directory at LBA 0
    bool fullDump = false;     // true for a real 16 KB PIC binary
    uint32_t crc32 = 0;        // CRC32 of the whole file
    uint32_t size = 0;
};

bool parseNaomiPic(const std::vector<uint8_t>& data, NaomiPic& out, std::string& error);

// Known PIC for the supported game (from MAME/Flycast's ROM tables).
constexpr uint32_t kGds0033PicCrc32 = 0x081ccd51;   // 317-0384-com.pic, Initial D3 Export
constexpr const char* kGds0033PicName = "317-0384-com.pic";

} // namespace idas3
