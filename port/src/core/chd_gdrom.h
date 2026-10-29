// GD-ROM access on top of a MAME CHD image (libchdr).
//
// A NAOMI GD-ROM CHD stores every track as 2448-byte frames (2352 sector +
// 96 subcode), each track padded to a multiple of 4 frames. Track 3 holds
// the high-density area, which starts at FAD 45150 (LBA 45000). This class
// maps a FAD to its CHD hunk/frame and returns the 2048 bytes of user data.
//
// The track layout logic mirrors Flycast's core/imgread/chd.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct _chd_file;

namespace idas3 {

struct GdTrack
{
    int number = 0;
    std::string type;          // "MODE1_RAW", "AUDIO", ...
    uint32_t frames = 0;       // frames stored in the CHD for this track (incl. pad)
    uint32_t padFrames = 0;    // GD-ROM "PAD:" frames at the end of the track
    uint32_t startFad = 0;     // first FAD of the track
    uint32_t endFad = 0;       // last FAD holding real data
    uint32_t sectorSize = 0;   // 2048 / 2336 / 2352
    uint32_t chdFrame = 0;     // index of the track's first frame inside the CHD
    bool isData = false;
};

class ChdGdrom
{
public:
    ChdGdrom() = default;
    ~ChdGdrom();
    ChdGdrom(const ChdGdrom&) = delete;
    ChdGdrom& operator=(const ChdGdrom&) = delete;

    bool open(const char* path, std::string& error);
    void close();
    bool isOpen() const { return chd_ != nullptr; }

    // Reads `count` consecutive 2048-byte user-data sectors starting at FAD.
    bool readSectors(uint32_t fad, uint32_t count, uint8_t* dst);

    const std::vector<GdTrack>& tracks() const { return tracks_; }
    bool isGdrom() const { return isGdrom_; }
    uint32_t version() const { return version_; }
    uint32_t hunkBytes() const { return hunkBytes_; }
    uint32_t totalHunks() const { return totalHunks_; }
    uint32_t totalFrames() const { return totalFrames_; }
    uint64_t logicalBytes() const { return logicalBytes_; }
    const std::string& codecs() const { return codecs_; }
    const uint8_t* sha1() const { return sha1_; }
    std::string sha1Hex() const;

    // Counters, for the log / speed diagnostics.
    uint32_t hunksDecoded() const { return hunksDecoded_; }

private:
    bool readUserData(uint32_t fad, uint8_t* dst);
    const uint8_t* fetchHunk(uint32_t hunk);
    const GdTrack* trackForFad(uint32_t fad) const;

    _chd_file* chd_ = nullptr;
    std::vector<GdTrack> tracks_;
    bool isGdrom_ = false;
    uint32_t version_ = 0;
    uint32_t hunkBytes_ = 0;
    uint32_t totalHunks_ = 0;
    uint32_t framesPerHunk_ = 0;
    uint32_t totalFrames_ = 0;
    uint64_t logicalBytes_ = 0;
    std::string codecs_;
    uint8_t sha1_[20] = {};

    // Tiny round-robin hunk cache. Reads are mostly sequential; a few slots
    // cover the ISO9660 lookups interleaved with file reads.
    static constexpr int kCacheSlots = 4;
    std::vector<uint8_t> cache_[kCacheSlots];
    uint32_t cacheHunk_[kCacheSlots] = {~0u, ~0u, ~0u, ~0u};
    int cacheNext_ = 0;
    uint32_t hunksDecoded_ = 0;
};

} // namespace idas3
