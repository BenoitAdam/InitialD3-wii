#include "chd_gdrom.h"
#include "log.h"

#include <libchdr/chd.h>

#include <cstdio>
#include <cstring>

namespace idas3 {

static constexpr uint32_t kFrameBytes = 2352 + 96;
static constexpr uint32_t kTrackPadding = 4;

ChdGdrom::~ChdGdrom()
{
    close();
}

void ChdGdrom::close()
{
    if (chd_)
    {
        chd_close(chd_);
        chd_ = nullptr;
    }
    tracks_.clear();
    for (int i = 0; i < kCacheSlots; i++)
    {
        cache_[i].clear();
        cache_[i].shrink_to_fit();
        cacheHunk_[i] = ~0u;
    }
    cacheNext_ = 0;
    hunksDecoded_ = 0;
}

static uint32_t sectorSizeForType(const char* type)
{
    if (!strcmp(type, "AUDIO"))
        return 2352;
    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE1/2048"))
        return 2048;
    if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE1/2352"))
        return 2352;
    if (!strcmp(type, "MODE2") || !strcmp(type, "MODE2/2336"))
        return 2336;
    if (!strcmp(type, "MODE2_RAW") || !strcmp(type, "MODE2/2352") || !strcmp(type, "CDI/2352"))
        return 2352;
    return 0;
}

bool ChdGdrom::open(const char* path, std::string& error)
{
    close();

    chd_error err = chd_open(path, CHD_OPEN_READ, nullptr, &chd_);
    if (err != CHDERR_NONE)
    {
        chd_ = nullptr;
        error = std::string("cannot open CHD: ") + chd_error_string(err);
        return false;
    }

    const chd_header* head = chd_get_header(chd_);
    version_ = head->version;
    hunkBytes_ = head->hunkbytes;
    totalHunks_ = head->totalhunks;
    logicalBytes_ = head->logicalbytes;
    memcpy(sha1_, head->sha1, sizeof(sha1_));

    codecs_.clear();
    for (int i = 0; i < 4; i++)
    {
        if (head->compression[i] == CHD_CODEC_NONE)
            continue;
        if (!codecs_.empty())
            codecs_ += ' ';
        // Four-character codec tag, e.g. "cdlz".
        uint32_t c = head->compression[i];
        char tag[5] = {(char)(c >> 24), (char)(c >> 16), (char)(c >> 8), (char)c, 0};
        codecs_ += tag;
    }
    if (codecs_.empty())
        codecs_ = "none";

    if (hunkBytes_ == 0 || hunkBytes_ % kFrameBytes != 0)
    {
        error = "not a CD/GD-ROM CHD (hunk size is not a multiple of 2448 bytes)";
        close();
        return false;
    }
    framesPerHunk_ = hunkBytes_ / kFrameBytes;

    // Track table. Same parsing order as Flycast: CHT2, CHTR, then the two
    // GD-ROM specific tags.
    isGdrom_ = version_ < 5;
    uint32_t fad = 150;
    uint32_t chdFrame = 0;
    for (;;)
    {
        char meta[512];
        uint32_t metaLen = 0, tag = 0;
        uint8_t flags = 0;
        char type[32] = "", subtype[32] = "", pgtype[32] = "", pgsub[32] = "";
        int tkid = -1, frames = 0, pregap = 0, postgap = 0, padframes = 0;
        const uint32_t index = (uint32_t)tracks_.size();

        memset(meta, 0, sizeof(meta));
        err = chd_get_metadata(chd_, CDROM_TRACK_METADATA2_TAG, index, meta, sizeof(meta) - 1, &metaLen, &tag, &flags);
        if (err == CHDERR_NONE)
        {
            sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
        }
        else if ((err = chd_get_metadata(chd_, CDROM_TRACK_METADATA_TAG, index, meta, sizeof(meta) - 1, &metaLen, &tag, &flags)) == CHDERR_NONE)
        {
            sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames);
        }
        else
        {
            err = chd_get_metadata(chd_, GDROM_OLD_METADATA_TAG, index, meta, sizeof(meta) - 1, &metaLen, &tag, &flags);
            if (err != CHDERR_NONE)
                err = chd_get_metadata(chd_, GDROM_TRACK_METADATA_TAG, index, meta, sizeof(meta) - 1, &metaLen, &tag, &flags);
            if (err != CHDERR_NONE)
                break;
            sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames, &padframes, &pregap, pgtype, pgsub, &postgap);
            isGdrom_ = true;
        }
        log::write("[chd] %s\n", meta);

        if (tkid != (int)index + 1)
        {
            error = "unexpected track number in CHD metadata";
            close();
            return false;
        }
        if (frames <= 0)
        {
            error = "track with no frames in CHD metadata";
            close();
            return false;
        }
        if (strcmp(subtype, "NONE") != 0 || pregap != 0 || postgap != 0)
        {
            error = "unsupported track subtype/pregap/postgap in CHD";
            close();
            return false;
        }

        GdTrack t;
        t.number = tkid;
        t.type = type;
        t.frames = (uint32_t)frames;
        t.padFrames = (uint32_t)padframes;
        t.startFad = fad;
        t.endFad = fad + (uint32_t)frames - 1 - (uint32_t)padframes;
        t.sectorSize = sectorSizeForType(type);
        t.chdFrame = chdFrame;
        t.isData = strcmp(type, "AUDIO") != 0;
        if (t.sectorSize == 0)
        {
            error = "unsupported track type '" + t.type + "'";
            close();
            return false;
        }
        tracks_.push_back(t);

        fad += (uint32_t)frames;
        chdFrame += ((uint32_t)frames + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
    }
    totalFrames_ = fad;

    if (tracks_.empty())
    {
        error = "CHD has no CD/GD-ROM track metadata";
        close();
        return false;
    }
    if (!isGdrom_ || tracks_.size() < 3)
    {
        error = "CHD is a CD-ROM, not a GD-ROM (NAOMI games are GD-ROMs)";
        close();
        return false;
    }
    if (totalFrames_ != 549300)
        log::write("[chd] warning: %u total frames, 549300 expected for a GD-ROM\n", totalFrames_);

    for (int i = 0; i < kCacheSlots; i++)
        cache_[i].resize(hunkBytes_);
    return true;
}

std::string ChdGdrom::sha1Hex() const
{
    char buf[41];
    for (int i = 0; i < 20; i++)
        snprintf(buf + i * 2, 3, "%02x", sha1_[i]);
    return std::string(buf, 40);
}

const GdTrack* ChdGdrom::trackForFad(uint32_t fad) const
{
    for (const GdTrack& t : tracks_)
        if (fad >= t.startFad && fad < t.startFad + t.frames)
            return &t;
    return nullptr;
}

const uint8_t* ChdGdrom::fetchHunk(uint32_t hunk)
{
    for (int i = 0; i < kCacheSlots; i++)
        if (cacheHunk_[i] == hunk)
            return cache_[i].data();

    int slot = cacheNext_;
    cacheNext_ = (cacheNext_ + 1) % kCacheSlots;
    cacheHunk_[slot] = ~0u;
    if (chd_read(chd_, hunk, cache_[slot].data()) != CHDERR_NONE)
    {
        log::write("[chd] chd_read failed for hunk %u\n", hunk);
        return nullptr;
    }
    cacheHunk_[slot] = hunk;
    hunksDecoded_++;
    return cache_[slot].data();
}

bool ChdGdrom::readUserData(uint32_t fad, uint8_t* dst)
{
    const GdTrack* t = trackForFad(fad);
    if (!t || !t->isData)
        return false;

    const uint32_t frame = fad - t->startFad + t->chdFrame;
    const uint32_t hunk = frame / framesPerHunk_;
    const uint32_t inHunk = frame % framesPerHunk_;
    if (hunk >= totalHunks_)
        return false;

    const uint8_t* h = fetchHunk(hunk);
    if (!h)
        return false;
    const uint8_t* sector = h + inHunk * kFrameBytes;

    uint32_t userOffset = 0;
    if (t->sectorSize == 2352)
        userOffset = (sector[15] == 2) ? 24 : 16;   // mode 2 form 1 has an 8-byte subheader
    else if (t->sectorSize == 2336)
        userOffset = 8;
    memcpy(dst, sector + userOffset, 2048);
    return true;
}

bool ChdGdrom::readSectors(uint32_t fad, uint32_t count, uint8_t* dst)
{
    for (uint32_t i = 0; i < count; i++)
        if (!readUserData(fad + i, dst + i * 2048))
            return false;
    return true;
}

} // namespace idas3
