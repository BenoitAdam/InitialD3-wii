#include "dimm_image.h"
#include "bytes.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace idas3 {

// ISO9660 names on these discs are at most 24 characters (MAME FILENAME_LENGTH).
static constexpr int kIsoNameMax = 24;
// The GD-ROM high-density area starts at LBA 45000 (FAD 45150).
static constexpr uint32_t kHdAreaLba = 45000;

std::vector<IsoEntry> parseIsoDirectorySector(const uint8_t* s)
{
    std::vector<IsoEntry> out;
    for (uint32_t pos = 0; pos < 2048 && s[pos] != 0; pos += s[pos])
    {
        const uint8_t recLen = s[pos];
        if (recLen < 34 || pos + recLen > 2048)
            break;
        IsoEntry e;
        e.lba = rd_le32(s + pos + 2);
        e.size = rd_le32(s + pos + 10);
        e.isDir = (s[pos + 25] & 2) != 0;
        const int len = std::min<int>(s[pos + 32], kIsoNameMax);
        for (int i = 0; i < len; i++)
        {
            char c = (char)s[pos + 33 + i];
            if (c == ';')
                break;
            if (c == 0 && i == 0 && len == 1)
            {
                e.name = ".";
                break;
            }
            if (c == 1 && i == 0 && len == 1)
            {
                e.name = "..";
                break;
            }
            e.name += c;
        }
        out.push_back(e);
    }
    return out;
}

bool isoNameMatches(const std::string& fileName, const char* pattern)
{
    if (pattern[0] == '*')
    {
        // Flycast: strchr(fname, pattern[1]) then compare the remainder.
        const char* p = strchr(fileName.c_str(), pattern[1]);
        return p != nullptr && strcmp(p, pattern + 1) == 0;
    }
    return fileName == pattern;
}

static bool findFile(const std::vector<IsoEntry>& dir, const char* pattern, uint32_t& lba, uint32_t& size,
                     std::string& found)
{
    for (const IsoEntry& e : dir)
    {
        if (e.isDir)
            continue;
        if (isoNameMatches(e.name, pattern))
        {
            lba = e.lba;
            size = e.size;
            found = e.name;
            return true;
        }
    }
    return false;
}

bool DimmImage::readHd(uint32_t lba, uint8_t* dst)
{
    return gdrom_->readSectors(lba + 150, 1, dst);
}

void DimmImage::close()
{
    gdrom_ = nullptr;
    fileName_.clear();
    fileLba_ = fileSize_ = 0;
    rootEntries_.clear();
    run_.clear();
    run_.shrink_to_fit();
    runFirst_ = ~0u;
    runCount_ = 0;
}

bool DimmImage::open(ChdGdrom& gdrom, const NaomiPic& pic, std::string& error)
{
    close();
    gdrom_ = &gdrom;

    uint8_t buffer[2048];
    uint8_t dirSector[2048];

    // Primary volume descriptor. The DIMM firmware starts from this frame.
    const uint32_t base = pic.netPic ? 0 : kHdAreaLba;
    if (!readHd(base + 16, buffer))
    {
        error = "cannot read the GD-ROM volume descriptor";
        close();
        return false;
    }
    if (memcmp(buffer + 1, "CD001", 5) != 0)
        log::write("[dimm] warning: no ISO9660 signature at LBA %u\n", base + 16);

    const uint32_t pathTable = rd_le32(buffer + 0x8c);
    if (!readHd(pathTable, buffer))
    {
        error = "cannot read the GD-ROM path table";
        close();
        return false;
    }

    std::string name = pic.fileName;
    if (!pic.netPic)
    {
        const uint32_t dir = rd_le32(buffer + 2);
        if (!readHd(dir, dirSector))
        {
            error = "cannot read the GD-ROM root directory";
            close();
            return false;
        }
        rootEntries_ = parseIsoDirectorySector(dirSector);

        // The PIC may name a 256-byte descriptor file whose offset 0xC0 holds
        // the real image name.
        uint32_t lba = 0, size = 0;
        std::string found;
        if (findFile(rootEntries_, name.c_str(), lba, size, found) && size == 0x100)
        {
            if (!readHd(lba, buffer))
            {
                error = "cannot read the game descriptor file";
                close();
                return false;
            }
            char real[kIsoNameMax] = {};
            memcpy(real, buffer + 0xc0, kIsoNameMax - 1);
            log::write("[dimm] descriptor %s -> %s\n", found.c_str(), real);
            name = real;
        }
    }
    else
    {
        // Net-boot discs keep the image in a ROM/ directory.
        bool foundRom = false;
        for (uint32_t i = 0; i < 2048 && buffer[i] != 0;)
        {
            if (buffer[i] == 3 && buffer[i + 8] == 'R' && buffer[i + 9] == 'O' && buffer[i + 10] == 'M')
            {
                const uint32_t dir = rd_le32(buffer + i + 2);
                name = "ROM.BIN";
                if (!readHd(dir, dirSector))
                {
                    error = "cannot read the GD-ROM ROM directory";
                    close();
                    return false;
                }
                foundRom = true;
                break;
            }
            i += buffer[i] + 8 + (buffer[i] & 1);
        }
        if (!foundRom)
        {
            const uint32_t dir = rd_le32(buffer + 2);
            if (!readHd(dir, dirSector))
            {
                error = "cannot read the GD-ROM root directory";
                close();
                return false;
            }
        }
        rootEntries_ = parseIsoDirectorySector(dirSector);
    }

    for (const IsoEntry& e : rootEntries_)
        log::write("[dimm] root: %-24s lba %7u size %10u%s\n", e.name.c_str(), e.lba, e.size, e.isDir ? " <DIR>" : "");

    uint32_t lba = 0, size = 0;
    std::string found;
    if (!findFile(rootEntries_, name.c_str(), lba, size, found))
    {
        // mj1-style discs: wrong name in the PIC, take the first .BIN.
        if (!findFile(rootEntries_, "*.BIN", lba, size, found))
        {
            error = "the game file '" + name + "' named by the PIC is not on this GD-ROM "
                    "(wrong PIC for this disc?)";
            close();
            return false;
        }
        log::write("[dimm] '%s' not found, using first .BIN '%s'\n", name.c_str(), found.c_str());
    }

    fileName_ = found;
    fileLba_ = lba;
    fileSize_ = size;
    des_.setKey(pic.desKey);
    run_.resize(kRunSectors * 2048);
    log::write("[dimm] game image %s: lba %u, %u bytes\n", fileName_.c_str(), fileLba_, fileSize_);
    return true;
}

bool DimmImage::read(uint32_t offset, uint32_t length, uint8_t* dst)
{
    if (!gdrom_)
        return false;

    while (length > 0)
    {
        if (offset >= fileSize_)
        {
            memset(dst, 0, length);
            return true;
        }

        const uint32_t sector = offset / 2048;
        if (sector < runFirst_ || sector >= runFirst_ + runCount_)
        {
            // Refill the run, aligned so sequential reads hit whole CHD hunks.
            const uint32_t first = sector - (sector % kRunSectors);
            const uint32_t fileSectors = (fileSize_ + 2047) / 2048;
            const uint32_t count = std::min<uint32_t>(kRunSectors, fileSectors - first);
            runFirst_ = ~0u;
            if (!gdrom_->readSectors(fileLba_ + first + 150, count, run_.data()))
            {
                log::write("[dimm] read failed at image offset 0x%08x\n", offset);
                return false;
            }
            des_.decryptBuffer(run_.data(), count * 2048);
            runFirst_ = first;
            runCount_ = count;
        }

        const uint32_t runOffset = offset - runFirst_ * 2048;
        uint32_t chunk = runCount_ * 2048 - runOffset;
        chunk = std::min(chunk, length);
        chunk = std::min(chunk, fileSize_ - offset);
        memcpy(dst, run_.data() + runOffset, chunk);
        dst += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool DimmImage::extract(const char* path, const std::function<bool(uint32_t, uint32_t)>& progress,
                        std::string& error)
{
    if (!gdrom_)
    {
        error = "no game image open";
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (!f)
    {
        error = std::string("cannot create ") + path;
        return false;
    }
    std::vector<uint8_t> buf(256 * 1024);
    for (uint32_t done = 0; done < fileSize_;)
    {
        const uint32_t chunk = std::min<uint32_t>((uint32_t)buf.size(), fileSize_ - done);
        if (!read(done, chunk, buf.data()) || fwrite(buf.data(), 1, chunk, f) != chunk)
        {
            fclose(f);
            remove(path);
            error = "read or write error during extraction";
            return false;
        }
        done += chunk;
        if (progress && !progress(done, fileSize_))
        {
            fclose(f);
            remove(path);
            error = "cancelled";
            return false;
        }
    }
    fclose(f);
    return true;
}

} // namespace idas3
