#include "bios_image.h"
#include "game_session.h"
#include "log.h"
#include "../platform/platform.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace idas3 {

namespace {

constexpr size_t kRomSize = 2u << 20;
constexpr size_t kLibraryEnd = 0x60000 + 0x7000;   // the service library must be inside
// Only the start of the ROM is kept: the game reads nothing but the service
// library (0x60000-0x67000). On the Wii every byte the launcher's heap ever
// held is lost to the guest page pool (newlib never returns memory to the
// arena), so the ZIP is streamed rather than loaded: <1 MB peak instead of
// the whole 9 MB naomi.zip plus a 2 MB image.
constexpr size_t kKeptBytes = 512u << 10;

// Preferred ROMs inside the MAME sets, best first. Any NAOMI / NAOMI 2 BIOS
// works for the service library; export and Japan revisions are listed first
// because they are the ones tested.
const char* const kPreferred[] = {
    "epr-23605c.ic27", "epr-23605b.ic27", "epr-23605a.ic27", "epr-23605.ic27",   // NAOMI 2
    "epr-21578h.ic27", "epr-21576h.ic27", "epr-21577h.ic27",                      // NAOMI
    "epr-21578g.ic27", "epr-21576g.ic27", "epr-21577g.ic27",
};

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

// The first `maxBytes` of a file (a raw ROM dump).
bool readFileHead(const std::string& path, size_t maxBytes, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.resize(maxBytes);
    out.resize(fread(out.data(), 1, maxBytes, f));
    fclose(f);
    return !out.empty();
}

bool readAt(FILE* f, long offset, void* dst, size_t bytes)
{
    return fseek(f, offset, SEEK_SET) == 0 && fread(dst, 1, bytes, f) == bytes;
}

struct ZipEntry
{
    std::string name;
    uint16_t method = 0;
    uint32_t compressed = 0, size = 0, localOffset = 0;
};

// Central directory, read from the end of the file.
bool zipEntries(FILE* f, std::vector<ZipEntry>& out)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return false;
    const long fileSize = ftell(f);
    if (fileSize < 22)
        return false;
    const long tailSize = fileSize < 65557 ? fileSize : 65557;
    std::vector<uint8_t> tail((size_t)tailSize);
    if (!readAt(f, fileSize - tailSize, tail.data(), tail.size()))
        return false;
    size_t eocd = tail.size() - 22;
    while (le32(&tail[eocd]) != 0x06054B50)
    {
        if (eocd == 0)
            return false;
        eocd--;
    }
    const uint16_t count = le16(&tail[eocd + 10]);
    const uint32_t dirSize = le32(&tail[eocd + 12]);
    const uint32_t dirOffset = le32(&tail[eocd + 16]);
    if ((long)dirOffset + (long)dirSize > fileSize || dirSize > (4u << 20))
        return false;
    std::vector<uint8_t> dir(dirSize);
    if (!readAt(f, dirOffset, dir.data(), dir.size()))
        return false;
    size_t pos = 0;
    for (uint16_t i = 0; i < count; i++)
    {
        if (pos + 46 > dir.size() || le32(&dir[pos]) != 0x02014B50)
            return false;
        ZipEntry e;
        e.method = le16(&dir[pos + 10]);
        e.compressed = le32(&dir[pos + 20]);
        e.size = le32(&dir[pos + 24]);
        const uint16_t nameLen = le16(&dir[pos + 28]);
        const uint16_t extraLen = le16(&dir[pos + 30]);
        const uint16_t commentLen = le16(&dir[pos + 32]);
        e.localOffset = le32(&dir[pos + 42]);
        if (pos + 46 + nameLen > dir.size())
            return false;
        e.name.assign((const char*)&dir[pos + 46], nameLen);
        out.push_back(e);
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

// The first `keep` bytes of entry `e`, inflated in 64 KB steps.
bool zipExtract(FILE* f, const ZipEntry& e, size_t keep, std::vector<uint8_t>& out)
{
    uint8_t lh[30];
    if (!readAt(f, e.localOffset, lh, sizeof(lh)) || le32(lh) != 0x04034B50)
        return false;
    const long data = (long)e.localOffset + 30 + le16(&lh[26]) + le16(&lh[28]);
    if (keep > e.size)
        keep = e.size;
    out.resize(keep);
    if (e.method == 0)
        return e.compressed == e.size && readAt(f, data, out.data(), keep);
    if (e.method != 8 || fseek(f, data, SEEK_SET) != 0)
        return false;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return false;
    std::vector<uint8_t> in(64u << 10);
    uint32_t left = e.compressed;
    zs.next_out = out.data();
    zs.avail_out = (uInt)keep;
    int rc = Z_OK;
    while (zs.avail_out > 0 && rc == Z_OK)
    {
        if (zs.avail_in == 0)
        {
            const size_t n = left < in.size() ? left : in.size();
            if (n == 0 || fread(in.data(), 1, n, f) != n)
                break;
            left -= (uint32_t)n;
            zs.next_in = in.data();
            zs.avail_in = (uInt)n;
        }
        rc = inflate(&zs, Z_NO_FLUSH);
    }
    inflateEnd(&zs);
    return zs.total_out == keep;
}

bool usableRom(const std::vector<uint8_t>& rom)
{
    return rom.size() >= kLibraryEnd;
}

bool fromZip(const std::string& path, BiosImage& out, std::string& error)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
    {
        error = "cannot read " + pathFileName(path);
        return false;
    }
    std::vector<ZipEntry> entries;
    if (!zipEntries(f, entries))
    {
        fclose(f);
        error = pathFileName(path) + " is not a valid ZIP file";
        return false;
    }
    const ZipEntry* pick = nullptr;
    for (const char* want : kPreferred)
    {
        for (const ZipEntry& e : entries)
            if (toLower(e.name) == want)
            {
                pick = &e;
                break;
            }
        if (pick)
            break;
    }
    if (!pick)
        for (const ZipEntry& e : entries)
        {
            const std::string n = toLower(e.name);
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".ic27") == 0 && e.size == kRomSize)
            {
                pick = &e;
                break;
            }
        }
    if (!pick)
    {
        fclose(f);
        error = pathFileName(path) + " holds no NAOMI BIOS ROM (*.ic27)";
        return false;
    }
    const bool ok = zipExtract(f, *pick, kKeptBytes, out.data) && usableRom(out.data);
    fclose(f);
    if (!ok)
    {
        error = "cannot extract " + pick->name + " from " + pathFileName(path);
        return false;
    }
    out.source = pathFileName(path) + ":" + pick->name;
    return true;
}

} // namespace

bool loadBios(const std::string& path, BiosImage& out, std::string& error)
{
    const std::string lower = toLower(path);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0)
        return fromZip(path, out, error);
    if (!readFileHead(path, kKeptBytes, out.data) || !usableRom(out.data))
    {
        error = "cannot read " + pathFileName(path) + " as a BIOS ROM";
        return false;
    }
    out.source = pathFileName(path);
    return true;
}

bool findBios(const std::vector<std::string>& folders, BiosImage& out, std::string& error)
{
    std::string lastError;
    for (const char* zipName : {"naomi2.zip", "naomi.zip"})
        for (const std::string& dir : folders)
        {
            const std::string path = dir + zipName;
            if (platform::fileExists(path))
            {
                if (fromZip(path, out, lastError))
                    return true;
                log::write("[bios] %s\n", lastError.c_str());
            }
        }
    for (const std::string& dir : folders)
    {
        std::vector<platform::DirEntry> entries;
        if (!platform::listDirectory(dir, entries))
            continue;
        for (const platform::DirEntry& e : entries)
        {
            const std::string n = toLower(e.name);
            if (!e.isDir && n.size() > 5 && n.compare(n.size() - 5, 5, ".ic27") == 0)
            {
                if (loadBios(dir + e.name, out, lastError))
                    return true;
                log::write("[bios] %s\n", lastError.c_str());
            }
        }
    }
    error = lastError.empty() ? "no NAOMI BIOS found: put naomi.zip (or naomi2.zip, or an .ic27 BIOS ROM) next "
                                "to the CHD"
                              : lastError;
    return false;
}

} // namespace idas3
