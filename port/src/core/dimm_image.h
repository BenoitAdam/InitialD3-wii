// The decrypted game image, as the NAOMI DIMM board presents it to the game.
//
// On real hardware the DIMM board copies the encrypted file from the GD-ROM
// into its RAM and decrypts it. Flycast keeps the whole decrypted image in
// host memory; that is impossible on the Wii (88 MB total RAM), so this class
// decrypts on demand straight from the CHD: any (offset, length) read is
// served by reading the covering GD-ROM sectors and DES-decrypting them.
#pragma once

#include "chd_gdrom.h"
#include "naomi_des.h"
#include "naomi_pic.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace idas3 {

struct IsoEntry
{
    std::string name;
    uint32_t lba = 0;
    uint32_t size = 0;
    bool isDir = false;
};

class DimmImage
{
public:
    // Locates the encrypted game file named by the PIC and prepares the key.
    bool open(ChdGdrom& gdrom, const NaomiPic& pic, std::string& error);
    void close();
    bool isOpen() const { return gdrom_ != nullptr; }

    // Decrypted read. Bytes past the end of the file read as zero, like the
    // DIMM RAM past the loaded image.
    bool read(uint32_t offset, uint32_t length, uint8_t* dst);

    uint32_t size() const { return fileSize_; }
    uint32_t fileLba() const { return fileLba_; }
    const std::string& fileName() const { return fileName_; }
    const std::vector<IsoEntry>& rootEntries() const { return rootEntries_; }

    // Writes the whole decrypted image to `path`. The callback receives
    // (bytesDone, bytesTotal) and may return false to cancel.
    bool extract(const char* path, const std::function<bool(uint32_t, uint32_t)>& progress,
                 std::string& error);

    // Read-ahead: runs are kept in a small cache, and a run that follows one
    // still there (a stream) gets the next decoded before it is asked for. On a
    // Wii by a thread under the emulation's priority; elsewhere at once. Off:
    // one run, decoded when asked for.
    void setReadAhead(bool on);
    bool readAheadBusy() const { return jobsQueued_ != jobsDone_; }

    struct ReadStats
    {
        uint32_t runsAsked;     // decoded when asked for
        uint32_t runsAhead;     // found decoded ahead
        uint32_t runsLate;      // of those, the caller waited for the thread
        uint32_t runsStarted;   // given to the read-ahead
    };
    const ReadStats& readStats() const { return stats_; }

private:
    struct Run
    {
        uint32_t first = ~0u;           // first sector
        uint32_t count = 0;
        uint32_t used = 0;              // last use, by useClock_
        bool fresh = false;             // decoded ahead, not read yet
        volatile bool busy = false;     // the thread is decoding it
        volatile bool ok = false;       // holds its data
    };

    bool readHd(uint32_t lba, uint8_t* dst2048);
    bool decodeRun(uint32_t first, uint32_t count, uint8_t* dst);
    uint8_t* runData(const Run& run) { return run_.data() + (&run - runs_) * (kRunSectors * 2048); }
    Run* findRun(uint32_t first);
    Run* oldestRun(const Run* keep);
    Run* useRun(uint32_t first);
    void aheadStart(uint32_t first, const Run* reading);
    static void* aheadMain(void* self);

    ChdGdrom* gdrom_ = nullptr;
    NaomiDes des_;
    std::string fileName_;
    uint32_t fileLba_ = 0;
    uint32_t fileSize_ = 0;
    std::vector<IsoEntry> rootEntries_;

    // Decrypted runs of sectors. Two streams alternate in a race: with one run
    // kept, each threw out the other's and every read decoded again. A stream
    // reads across two runs (8 KB at odd offsets) and has the next one ahead,
    // so it holds three; with four in all they threw each other's out.
    static constexpr uint32_t kRunSectors = 8;
    static constexpr int kRuns = 6;
    std::vector<uint8_t> run_;          // runSlots_ runs
    Run runs_[kRuns];
    int runSlots_ = 1;
    uint32_t useClock_ = 0;

    uint8_t jobs_[kRuns] = {};          // runs for the thread, a ring
    volatile uint32_t jobsQueued_ = 0;  // written by the caller only
    volatile uint32_t jobsDone_ = 0;    // written by the thread only
    volatile bool aheadStop_ = false;
    ReadStats stats_ = {};
};

// Parses one 2048-byte ISO9660 directory sector (exposed for tests).
std::vector<IsoEntry> parseIsoDirectorySector(const uint8_t* sector);

// MAME/Flycast name matching: exact, or "*.EXT" style suffix match.
bool isoNameMatches(const std::string& fileName, const char* pattern);

} // namespace idas3
