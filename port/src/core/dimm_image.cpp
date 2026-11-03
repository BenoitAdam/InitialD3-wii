#include "dimm_image.h"
#include "bytes.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(GEKKO)
#include <gccore.h>
#include <unistd.h>
#endif

namespace idas3 {

// ISO9660 names on these discs are at most 24 characters (MAME FILENAME_LENGTH).
static constexpr int kIsoNameMax = 24;
// The GD-ROM high-density area starts at LBA 45000 (FAD 45150).
static constexpr uint32_t kHdAreaLba = 45000;

#if defined(GEKKO)
// Under the emulation (64): it runs when the emulation sleeps, waits for it, or
// gives it a slice at vblank (naomi_CartSlice). Above, it would stop the
// emulation for all of a decode, the stutter this is here to remove.
static constexpr u32 kAheadPrio = 32;
static constexpr u32 kAheadStack = 32 * 1024;
static lwp_t s_aheadThread = LWP_THREAD_NULL;
static sem_t s_aheadSem;
static mutex_t s_chdLock;
#endif

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

bool DimmImage::decodeRun(uint32_t first, uint32_t count, uint8_t* dst)
{
    // The CHD has one reader at a time. The emulation waiting here lets the
    // thread, under its priority, run and finish.
#if defined(GEKKO)
    const bool threaded = s_aheadThread != LWP_THREAD_NULL;
    if (threaded)
        LWP_MutexLock(s_chdLock);
#endif
    const bool ok = gdrom_->readSectors(fileLba_ + first + 150, count, dst);
#if defined(GEKKO)
    if (threaded)
        LWP_MutexUnlock(s_chdLock);
#endif
    if (ok)
        des_.decryptBuffer(dst, count * 2048);
    return ok;
}

void* DimmImage::aheadMain(void* self)
{
#if defined(GEKKO)
    DimmImage* d = static_cast<DimmImage*>(self);
    u32 job = 0;
    for (;;)
    {
        LWP_SemWait(s_aheadSem);
        if (d->aheadStop_)
            return nullptr;
        Run& run = d->runs_[d->jobs_[job++ % kRuns]];
        run.ok = d->decodeRun(run.first, run.count, d->runData(run));
        run.busy = false;
        d->jobsDone_ = d->jobsDone_ + 1;
    }
#else
    (void)self;
    return nullptr;
#endif
}

void DimmImage::setReadAhead(bool on)
{
    if (on == (runSlots_ > 1))
        return;
#if defined(GEKKO)
    if (on)
    {
        LWP_SemInit(&s_aheadSem, 0, kRuns);
        LWP_MutexInit(&s_chdLock, false);
        if (LWP_CreateThread(&s_aheadThread, aheadMain, this, nullptr, kAheadStack, kAheadPrio) < 0)
        {
            s_aheadThread = LWP_THREAD_NULL;
            LWP_MutexDestroy(s_chdLock);
            LWP_SemDestroy(s_aheadSem);
            log::write("[dimm] no read-ahead: cannot create its thread\n");
            return;
        }
    }
    else
    {
        while (readAheadBusy())
            usleep(100);
        aheadStop_ = true;
        LWP_SemPost(s_aheadSem);
        LWP_JoinThread(s_aheadThread, nullptr);
        s_aheadThread = LWP_THREAD_NULL;
        LWP_MutexDestroy(s_chdLock);
        LWP_SemDestroy(s_aheadSem);
        aheadStop_ = false;
    }
#endif
    runSlots_ = on ? kRuns : 1;
    run_.resize(runSlots_ * kRunSectors * 2048);
    run_.shrink_to_fit();
    for (Run& r : runs_)
    {
        r.first = ~0u;
        r.used = 0;
        r.fresh = r.ok = false;
    }
}

DimmImage::Run* DimmImage::findRun(uint32_t first)
{
    for (int i = 0; i < runSlots_; i++)
        if (runs_[i].first == first)
            return &runs_[i];
    return nullptr;
}

// The run read longest ago that the thread is not working on, `keep` apart.
DimmImage::Run* DimmImage::oldestRun(const Run* keep)
{
    Run* oldest = nullptr;
    for (int i = 0; i < runSlots_; i++)
        if (!runs_[i].busy && &runs_[i] != keep && (!oldest || runs_[i].used < oldest->used))
            oldest = &runs_[i];
    return oldest;
}

// `reading`: the run the caller is about to read.
void DimmImage::aheadStart(uint32_t first, const Run* reading)
{
    const uint32_t fileSectors = (fileSize_ + 2047) / 2048;
    if (runSlots_ < 2 || first >= fileSectors || findRun(first))
        return;
    Run* run = oldestRun(reading);
    if (!run)
        return;
    run->first = first;
    run->count = std::min<uint32_t>(kRunSectors, fileSectors - first);
    run->used = ++useClock_;            // not the next one to go, unread as it is
    run->fresh = true;
    run->ok = false;
    stats_.runsStarted++;
#if defined(GEKKO)
    run->busy = true;
    jobs_[jobsQueued_ % kRuns] = (uint8_t)(run - runs_);
    jobsQueued_ = jobsQueued_ + 1;
    LWP_SemPost(s_aheadSem);
#else
    run->ok = decodeRun(first, run->count, runData(*run));
#endif
}

DimmImage::Run* DimmImage::useRun(uint32_t first)
{
    Run* run = findRun(first);
    if (run && run->busy)
    {
        stats_.runsLate++;
#if defined(GEKKO)
        while (run->busy)
            usleep(100);
#endif
    }
    if (run && run->ok)
    {
        run->used = ++useClock_;
        if (run->fresh)
        {
            // Decoded ahead and asked for: the stream goes on.
            run->fresh = false;
            stats_.runsAhead++;
            aheadStart(first + kRunSectors, run);
        }
        return run;
    }

    // Refill, aligned so sequential reads hit whole CHD hunks.
    if (!run)
        run = oldestRun(nullptr);
#if defined(GEKKO)
    while (!run)
    {
        usleep(100);                    // every run with the thread
        run = oldestRun(nullptr);
    }
#endif
    const uint32_t fileSectors = (fileSize_ + 2047) / 2048;
    const uint32_t count = std::min<uint32_t>(kRunSectors, fileSectors - first);
    stats_.runsAsked++;
    run->first = ~0u;
    run->ok = false;
    if (!decodeRun(first, count, runData(*run)))
        return nullptr;
    run->first = first;
    run->count = count;
    run->used = ++useClock_;
    run->fresh = false;
    run->ok = true;
    // The run before it is still here: a stream. In a race the game streams its
    // music from the image, two channels at once, 8 KB each every ~44 frames.
    if (first >= kRunSectors && findRun(first - kRunSectors))
        aheadStart(first + kRunSectors, run);
    return run;
}

void DimmImage::close()
{
    setReadAhead(false);
    gdrom_ = nullptr;
    fileName_.clear();
    fileLba_ = fileSize_ = 0;
    rootEntries_.clear();
    run_.clear();
    run_.shrink_to_fit();
    for (Run& r : runs_)
    {
        r.first = ~0u;
        r.used = 0;
        r.fresh = r.ok = false;
    }
    useClock_ = 0;
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
        const uint32_t first = sector - (sector % kRunSectors);
        Run* run = useRun(first);
        if (!run)
        {
            log::write("[dimm] read failed at image offset 0x%08x\n", offset);
            return false;
        }

        const uint32_t runOffset = offset - first * 2048;
        uint32_t chunk = run->count * 2048 - runOffset;
        chunk = std::min(chunk, length);
        chunk = std::min(chunk, fileSize_ - offset);
        memcpy(dst, runData(*run) + runOffset, chunk);
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
