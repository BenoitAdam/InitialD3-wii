#include "naomi_boot.h"
#include "bytes.h"

#include <cstring>

namespace idas3 {

static std::string fixedString(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n && p[i] != 0; i++)
        s += (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

static void parseLoadTable(const uint8_t* p, std::vector<NaomiLoadEntry>& out)
{
    for (int i = 0; i < 8; i++)
    {
        NaomiLoadEntry e;
        e.offset = rd_le32(p + i * 12 + 0);
        e.address = rd_le32(p + i * 12 + 4);
        e.size = rd_le32(p + i * 12 + 8);
        if (e.offset == 0xffffffff)
            break;
        out.push_back(e);
    }
}

bool NaomiBootHeader::isNaomi2() const
{
    return boardName.compare(0, 6, "Naomi2") == 0 || boardName.compare(0, 6, "NAOMI2") == 0;
}

std::string NaomiBootHeader::displayTitle() const
{
    if (!titles[2].empty())
        return titles[2];
    if (!titles[0].empty())
        return titles[0];
    for (const std::string& t : titles)
        if (!t.empty())
            return t;
    return "(untitled)";
}

bool parseNaomiBootHeader(const uint8_t* p, NaomiBootHeader& h, std::string& error)
{
    h = NaomiBootHeader();
    h.boardName = fixedString(p + 0x000, 16);
    h.vendorName = fixedString(p + 0x010, 32);
    for (int i = 0; i < 8; i++)
        h.titles[i] = fixedString(p + 0x030 + i * 32, 32);
    h.year = rd_le16(p + 0x130);
    h.month = p[0x132];
    h.day = p[0x133];
    h.gameId = std::string((const char*)p + 0x134, 4);
    h.romMode = rd_le16(p + 0x138);
    parseLoadTable(p + 0x360, h.gameLoad);
    parseLoadTable(p + 0x3c0, h.testLoad);
    h.gamePC = rd_le32(p + 0x420);
    h.testPC = rd_le32(p + 0x424);
    h.country = p[0x428];
    h.cabinet = p[0x429];
    h.resolution = p[0x42a];
    h.vertical = p[0x42b];
    h.serialId = p[0x42c];
    h.serviceMode = p[0x42d];

    if (memcmp(p, "NAOMI", 5) != 0 && memcmp(p, "Naomi2", 6) != 0)
    {
        error = "decrypted data is not a NAOMI game header (wrong PIC for this GD-ROM?)";
        return false;
    }
    if (h.gameLoad.empty())
    {
        error = "NAOMI header has an empty program load table";
        return false;
    }
    if ((h.gamePC & 0x1c000000) != 0x0c000000)
    {
        error = "NAOMI header entry point is outside system RAM";
        return false;
    }
    return true;
}

} // namespace idas3
