#include "game_session.h"
#include "log.h"
#include "../platform/platform.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace idas3 {

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

static size_t lastSeparator(const std::string& p)
{
    const size_t a = p.find_last_of('/');
    const size_t b = p.find_last_of('\\');
    if (a == std::string::npos)
        return b;
    if (b == std::string::npos)
        return a;
    return std::max(a, b);
}

std::string pathDirectory(const std::string& path)
{
    const size_t s = lastSeparator(path);
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}

std::string pathFileName(const std::string& path)
{
    const size_t s = lastSeparator(path);
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string toLower(std::string s)
{
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

static std::string parentDirectory(const std::string& dir)
{
    // dir ends with a separator: drop it, then cut after the previous one.
    if (dir.size() < 2)
        return std::string();
    std::string d = dir.substr(0, dir.size() - 1);
    if (!d.empty() && d.back() == ':')
        return std::string();   // "sd:" - already the root
    const size_t s = lastSeparator(d);
    return s == std::string::npos ? std::string() : d.substr(0, s + 1);
}

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        out.insert(out.end(), buf, buf + n);
        if (out.size() > maxBytes)
        {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static uint32_t fileCrc32(const std::string& path)
{
    std::vector<uint8_t> data;
    if (!readWholeFile(path, data, 64 * 1024))
        return 0;
    return (uint32_t)crc32(0L, data.data(), (uInt)data.size());
}

std::string GameSession::findPic(const std::string& chdPath)
{
    const std::string dir = pathDirectory(chdPath);
    const std::string parent = parentDirectory(dir);
    const std::string dirs[2] = {dir, parent};

    for (const std::string& d : dirs)
    {
        if (d.empty())
            continue;
        std::vector<platform::DirEntry> entries;
        if (!platform::listDirectory(d, entries))
            continue;

        std::string exact, anyPic, knownCrc;
        for (const platform::DirEntry& e : entries)
        {
            if (e.isDir)
                continue;
            const std::string lower = toLower(e.name);
            if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".pic") != 0)
                continue;
            if (lower == kGds0033PicName)
                exact = d + e.name;
            else if (fileCrc32(d + e.name) == kGds0033PicCrc32)
                knownCrc = d + e.name;
            else if (anyPic.empty())
                anyPic = d + e.name;
        }
        if (!exact.empty())
            return exact;
        if (!knownCrc.empty())
            return knownCrc;
        if (!anyPic.empty())
            return anyPic;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void GameSession::add(CheckStatus status, const std::string& text, const Progress& progress)
{
    static const char* tags[] = {"OK", "WARN", "FAIL", "INFO"};
    lines_.push_back({status, text});
    log::write("[check] [%s] %s\n", tags[(int)status], text.c_str());
    if (progress)
        progress(lines_, std::string());
}

void GameSession::close()
{
    memory_.release();
    dimm_.close();
    gdrom_.close();
    lines_.clear();
    checked_ = loaded_ = supportedGame_ = false;
    loadedBytes_ = programCrc_ = loadUs_ = 0;
}

static std::string fmt(const char* f, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

static std::string fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

static double seconds(uint64_t us)
{
    return (double)us / 1000000.0;
}

bool GameSession::check(const std::string& chdPath, const Progress& progress)
{
    close();
    chdPath_ = chdPath;
    log::write("[check] ===== %s =====\n", chdPath.c_str());

    // 1. CHD container.
    if (progress)
        progress(lines_, "Opening " + pathFileName(chdPath) + " ...");
    uint64_t t0 = platform::microseconds();
    std::string error;
    if (!gdrom_.open(chdPath.c_str(), error))
    {
        add(CheckStatus::Fail, pathFileName(chdPath) + ": " + error, progress);
        return false;
    }
    add(CheckStatus::Ok,
        fmt("CHD v%u: %u tracks, %u B hunks, codecs %s", gdrom_.version(),
            (unsigned)gdrom_.tracks().size(), gdrom_.hunkBytes(), gdrom_.codecs().c_str()),
        progress);
    log::write("[check] CHD sha1 %s, %llu logical bytes, opened in %.3f s\n", gdrom_.sha1Hex().c_str(),
               (unsigned long long)gdrom_.logicalBytes(), seconds(platform::microseconds() - t0));

    // 2. GD-ROM layout: the high-density area must start at FAD 45150.
    const GdTrack* hd = nullptr;
    for (const GdTrack& t : gdrom_.tracks())
        if (t.startFad == 45150 && t.isData)
            hd = &t;
    if (!hd)
    {
        add(CheckStatus::Fail, "No GD-ROM high-density data track at FAD 45150", progress);
        return false;
    }
    add(CheckStatus::Ok,
        fmt("GD-ROM high-density area: track %d, %s, %.1f MB", hd->number, hd->type.c_str(),
            (double)(hd->endFad - hd->startFad + 1) * 2048.0 / (1024.0 * 1024.0)),
        progress);

    // 3. Security PIC.
    picPath_ = findPic(chdPath);
    if (picPath_.empty())
    {
        add(CheckStatus::Fail, std::string("Security PIC not found: put ") + kGds0033PicName +
                                   " next to the CHD",
            progress);
        return false;
    }
    std::vector<uint8_t> picData;
    if (!readWholeFile(picPath_, picData, 64 * 1024) || !parseNaomiPic(picData, pic_, error))
    {
        add(CheckStatus::Fail, pathFileName(picPath_) + ": " + (error.empty() ? "unreadable" : error), progress);
        return false;
    }
    if (pic_.crc32 == kGds0033PicCrc32)
        add(CheckStatus::Ok, fmt("PIC %s: CRC32 %08x matches GDS-0033", pathFileName(picPath_).c_str(), pic_.crc32),
            progress);
    else
        add(CheckStatus::Warn,
            fmt("PIC %s: CRC32 %08x, GDS-0033 expects %08x", pathFileName(picPath_).c_str(), pic_.crc32,
                kGds0033PicCrc32),
            progress);

    // 4. Encrypted image named by the PIC.
    if (progress)
        progress(lines_, "Reading the GD-ROM file system ...");
    if (!dimm_.open(gdrom_, pic_, error))
    {
        add(CheckStatus::Fail, error, progress);
        return false;
    }
    add(CheckStatus::Ok,
        fmt("Encrypted image %s: %.1f MB", dimm_.fileName().c_str(), (double)dimm_.size() / (1024.0 * 1024.0)),
        progress);

    // 5. Decrypt and parse the boot header.
    if (progress)
        progress(lines_, "Decrypting the boot header ...");
    std::vector<uint8_t> head(kNaomiBootHeaderSize);
    if (!dimm_.read(0, kNaomiBootHeaderSize, head.data()))
    {
        add(CheckStatus::Fail, "Cannot read the game image from the CHD", progress);
        return false;
    }
    if (!parseNaomiBootHeader(head.data(), header_, error))
    {
        add(CheckStatus::Fail, error, progress);
        return false;
    }
    add(CheckStatus::Ok,
        fmt("Decrypted %s game: \"%s\" %04u/%02u/%02u", header_.boardName.c_str(), header_.displayTitle().c_str(),
            header_.year, header_.month, header_.day),
        progress);

    // 6. Is it the game this port targets?
    std::string allTitles;
    for (const std::string& t : header_.titles)
        allTitles += toLower(t) + "|";
    supportedGame_ = allTitles.find("initial d") != std::string::npos && header_.isNaomi2();
    if (supportedGame_)
        add(CheckStatus::Ok, "Game recognized: Initial D Arcade Stage 3 (NAOMI 2)", progress);
    else
        add(CheckStatus::Warn, "Not Initial D Arcade Stage 3: only GDS-0033 is targeted", progress);

    uint32_t programBytes = 0;
    for (const NaomiLoadEntry& e : header_.gameLoad)
        programBytes += e.size;
    add(CheckStatus::Info,
        fmt("Program: %u segment(s), %u KB, entry point 0x%08X", (unsigned)header_.gameLoad.size(),
            programBytes / 1024, header_.gamePC),
        progress);
    for (const NaomiLoadEntry& e : header_.gameLoad)
        log::write("[check] load: image 0x%08x -> 0x%08x, %u bytes\n", e.offset, e.address, e.size);
    log::write("[check] vendor '%s', id '%s', regions %02x, cabinet %02x\n", header_.vendorName.c_str(),
               header_.gameId.c_str(), header_.country, header_.cabinet);

    checked_ = true;
    return true;
}

bool GameSession::load(MemoryPlan plan, const Progress& progress)
{
    if (!checked_)
        return false;
    loaded_ = false;
    memory_.release();

    // 1. Guest memory.
    if (progress)
        progress(lines_, std::string("Allocating guest memory: ") + memoryPlanLabel(plan) + " ...");
    std::string error;
    if (!memory_.allocate(plan, error))
    {
        add(CheckStatus::Fail, std::string("Guest memory ") + memoryPlanLabel(plan) + ": " + error, progress);
        return false;
    }
    add(CheckStatus::Ok, std::string("Guest memory: ") + memoryPlanLabel(plan) + " allocated", progress);
    add(CheckStatus::Info, "Host memory left: " + platform::memoryReport(), progress);

    // 2. Program segments, exactly as the NAOMI BIOS copies them.
    uint32_t total = 0;
    for (const NaomiLoadEntry& e : header_.gameLoad)
        total += e.size;

    const uint64_t t0 = platform::microseconds();
    uint64_t lastProgress = 0;
    uint32_t done = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    const uint32_t kChunk = 64 * 1024;
    for (size_t i = 0; i < header_.gameLoad.size(); i++)
    {
        const NaomiLoadEntry& e = header_.gameLoad[i];
        uint8_t* dst = memory_.ramPointer(e.address, e.size);
        if (!dst)
        {
            add(CheckStatus::Fail, fmt("Segment %u: 0x%08X+%u is outside NAOMI RAM", (unsigned)i, e.address, e.size),
                progress);
            memory_.release();
            return false;
        }
        const uint32_t offset = e.offset & 0x1fffffff;
        for (uint32_t pos = 0; pos < e.size; pos += kChunk)
        {
            const uint32_t n = std::min(kChunk, e.size - pos);
            if (!dimm_.read(offset + pos, n, dst + pos))
            {
                add(CheckStatus::Fail, fmt("Read error in segment %u at offset 0x%08X", (unsigned)i, offset + pos),
                    progress);
                memory_.release();
                return false;
            }
            crc = crc32(crc, dst + pos, n);
            done += n;
            const uint64_t now = platform::microseconds();
            if (progress && now - lastProgress > 100000)
            {
                lastProgress = now;
                progress(lines_, fmt("Loading program: %u/%u KB (%u%%)", done / 1024, total / 1024,
                                     total ? (unsigned)((uint64_t)done * 100 / total) : 100));
            }
        }
    }
    loadUs_ = (uint32_t)(platform::microseconds() - t0);
    loadedBytes_ = done;
    programCrc_ = (uint32_t)crc;

    // 3. The BIOS leaves a copy of the boot header in RAM for the game.
    std::vector<uint8_t> head(kNaomiBootHeaderSize);
    if (dimm_.read(0, kNaomiBootHeaderSize, head.data()))
        if (uint8_t* h = memory_.ramPointer(kNaomiBootHeaderRamAddress, kNaomiBootHeaderSize))
            memcpy(h, head.data(), head.size());

    const double secs = seconds(loadUs_);
    add(CheckStatus::Ok,
        fmt("Program loaded: %u KB in %.2f s (%.0f KB/s), CRC32 %08x", done / 1024, secs,
            secs > 0 ? (double)done / 1024.0 / secs : 0.0, programCrc_),
        progress);
    add(CheckStatus::Info, fmt("Entry point 0x%08X is ready for the SH-4 runtime", header_.gamePC), progress);
    loaded_ = true;
    return true;
}

} // namespace idas3
