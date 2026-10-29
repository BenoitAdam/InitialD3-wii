// Synthetic NAOMI GD-ROM for tests: an uncompressed CHD v5 with the real
// GD-ROM track layout (high-density area at FAD 45150), an ISO9660 file
// system, a DES-encrypted fake game image and a matching 16 KB PIC.
// No game data is involved.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fixture {

struct Options
{
    uint64_t picKey = 0x0123456789abcdefULL;
    // When true the PIC names a 256-byte descriptor file whose offset 0xC0
    // holds the real image name (the indirection real discs use).
    bool useDescriptor = false;
    // Key used to encrypt the image (differs from picKey to simulate a wrong PIC).
    uint64_t encryptKey = 0x0123456789abcdefULL;
};

struct Fixture
{
    std::string chdPath;
    std::string picPath;
    std::vector<uint8_t> plainImage;   // the decrypted game image
    uint32_t gamePC = 0;
};

// Writes <dir>/gds-test.chd and <dir>/317-0384-com.pic.
bool build(const std::string& dir, const Options& options, Fixture& out, std::string& error);

} // namespace fixture
