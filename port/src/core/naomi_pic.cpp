#include "naomi_pic.h"

#include <zlib.h>

#include <cstring>

namespace idas3 {

bool parseNaomiPic(const std::vector<uint8_t>& d, NaomiPic& out, std::string& error)
{
    out = NaomiPic();
    out.size = (uint32_t)d.size();
    out.crc32 = (uint32_t)::crc32(0L, d.data(), (uInt)d.size());

    char name[16] = {};
    if (d.size() >= 0x4000)
    {
        // Real PIC binary: name and key are spread over every other byte.
        out.fullDump = true;
        for (int i = 0; i < 7; i++)
            name[i] = (char)d[0x7c0 + i * 2];
        for (int i = 0; i < 7; i++)
            name[i + 7] = (char)d[0x7e0 + i * 2];
        uint64_t key = 0;
        for (int i = 0; i < 7; i++)
            key |= (uint64_t)d[0x780 + i * 2] << (56 - i * 8);
        key |= d[0x7a0];
        out.desKey = key;
        out.netPic = d[0x6ee] != 0;
    }
    else if (d.size() >= 0x38)
    {
        // Extracted key data.
        memcpy(name, d.data() + 33, 7);
        memcpy(name + 7, d.data() + 25, 7);
        out.desKey = ((uint64_t)d[0x31] << 56) | ((uint64_t)d[0x32] << 48) |
                     ((uint64_t)d[0x33] << 40) | ((uint64_t)d[0x34] << 32) |
                     ((uint64_t)d[0x35] << 24) | ((uint64_t)d[0x36] << 16) |
                     ((uint64_t)d[0x37] << 8) | ((uint64_t)d[0x29]);
    }
    else
    {
        error = "PIC file is too small";
        return false;
    }

    // Names are space/NUL padded ISO9660 names.
    std::string n(name, strnlen(name, sizeof(name)));
    while (!n.empty() && (n.back() == ' ' || n.back() == '\0'))
        n.pop_back();
    if (n.empty())
    {
        error = "PIC does not contain a game file name";
        return false;
    }
    out.fileName = n;
    return true;
}

} // namespace idas3
