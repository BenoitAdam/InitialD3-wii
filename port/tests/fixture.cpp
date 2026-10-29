#include "fixture.h"
#include "core/bytes.h"
#include "core/naomi_des.h"

#include <cstdio>
#include <cstring>

namespace fixture {
namespace {

constexpr uint32_t kFrameBytes = 2448;
constexpr uint32_t kFramesPerHunk = 8;
constexpr uint32_t kHunkBytes = kFrameBytes * kFramesPerHunk;
constexpr uint32_t kTrack1Frames = 300;
constexpr uint32_t kTrack2Frames = 45000 - kTrack1Frames;   // track 3 starts at FAD 45150
constexpr uint32_t kHdLba = 45000;
constexpr uint32_t kImageSize = 200000;                     // multiple of 8, not of 2048

void be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void be64(uint8_t* p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

void dirRecord(uint8_t* sec, size_t& pos, const char* name, uint32_t lba, uint32_t size, bool isDir)
{
    const uint8_t nameLen = (uint8_t)(name[0] == '\0' || name[0] == '\1' ? 1 : strlen(name));
    uint8_t len = (uint8_t)(33 + nameLen);
    if (len & 1)
        len++;
    uint8_t* r = sec + pos;
    memset(r, 0, len);
    r[0] = len;
    idas3::wr_le32(r + 2, lba);
    be32(r + 6, lba);
    idas3::wr_le32(r + 10, size);
    be32(r + 14, size);
    r[25] = isDir ? 2 : 0;
    r[28] = 1;
    r[31] = 1;
    r[32] = nameLen;
    memcpy(r + 33, name, nameLen);
    pos += len;
}

void putString(uint8_t* p, const char* s, size_t field, char pad)
{
    memset(p, pad, field);
    memcpy(p, s, strlen(s));
}

std::vector<uint8_t> makePlainImage(uint32_t& gamePC)
{
    std::vector<uint8_t> img(kImageSize);
    for (uint32_t i = 0; i < kImageSize; i++)
        img[i] = (uint8_t)(i * 7 + (i >> 8) + (i >> 16));

    uint8_t* h = img.data();
    memset(h, 0, 0x500);
    putString(h + 0x000, "Naomi2", 16, ' ');
    putString(h + 0x010, "SEGA CORPORATION", 32, ' ');
    putString(h + 0x030 + 0 * 32, "INITIAL D TEST JAPAN", 32, ' ');
    putString(h + 0x030 + 1 * 32, "INITIAL D TEST USA", 32, ' ');
    putString(h + 0x030 + 2 * 32, "INITIAL D TEST EXPORT", 32, ' ');
    h[0x130] = 0xd4;   // 2004
    h[0x131] = 0x07;
    h[0x132] = 2;
    h[0x133] = 29;
    memcpy(h + 0x134, "TEST", 4);

    // Program load table: one segment through the P0 address, one through P1.
    const uint32_t table[3][3] = {
        {0x1000, 0x0c020000, 0x8000},
        {0x9000, 0x8c030000, 0x1234},
        {0xffffffff, 0, 0},
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            idas3::wr_le32(h + 0x360 + i * 12 + j * 4, table[i][j]);
    idas3::wr_le32(h + 0x3c0, 0xffffffff);
    gamePC = 0x0c021000;
    idas3::wr_le32(h + 0x420, gamePC);
    idas3::wr_le32(h + 0x424, 0x0c021000);
    h[0x428] = 0x0f;
    h[0x429] = 0x01;
    memset(h + 0x42e, 0xff, 0x500 - 0x42e);
    return img;
}

// One raw 2352-byte MODE1 sector + 96 subcode bytes.
void makeFrame(uint8_t* frame, uint32_t fad, const uint8_t* user)
{
    memset(frame, 0, kFrameBytes);
    frame[0] = 0;
    memset(frame + 1, 0xff, 10);
    frame[11] = 0;
    auto bcd = [](uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    frame[12] = bcd(fad / (60 * 75));
    frame[13] = bcd((fad / 75) % 60);
    frame[14] = bcd(fad % 75);
    frame[15] = 1;
    memcpy(frame + 16, user, 2048);
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

} // namespace

bool build(const std::string& dir, const Options& opt, Fixture& out, std::string& error)
{
    out = Fixture();
    out.plainImage = makePlainImage(out.gamePC);

    // --- Encrypt the image (whole sectors, like the GD-ROM file) ----------
    const uint32_t imageSectors = (kImageSize + 2047) / 2048;
    std::vector<uint8_t> enc(imageSectors * 2048, 0);
    memcpy(enc.data(), out.plainImage.data(), kImageSize);
    idas3::NaomiDes des;
    des.setKey(opt.encryptKey);
    for (size_t i = 0; i < enc.size(); i += 8)
        idas3::wr_le64(&enc[i], des.encryptBlock(idas3::rd_le64(&enc[i])));

    // --- ISO9660 in the high-density area (sector index = LBA - 45000) ----
    const uint32_t kPvd = 16, kPathTable = 18, kRootDir = 20, kDescriptor = 22, kImage = 24;
    uint32_t hdSectors = kImage + imageSectors;
    hdSectors = (hdSectors + kFramesPerHunk - 1) / kFramesPerHunk * kFramesPerHunk;
    std::vector<uint8_t> hd((size_t)hdSectors * 2048, 0);

    uint8_t* pvd = &hd[kPvd * 2048];
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    idas3::wr_le32(pvd + 0x8c, kHdLba + kPathTable);

    uint8_t* pt = &hd[kPathTable * 2048];
    pt[0] = 1;
    idas3::wr_le32(pt + 2, kHdLba + kRootDir);
    pt[6] = 1;

    const char* imageName = "IDTEST.BIN";
    uint8_t* root = &hd[kRootDir * 2048];
    size_t pos = 0;
    dirRecord(root, pos, "\0", kHdLba + kRootDir, 2048, true);
    dirRecord(root, pos, "\1", kHdLba + kRootDir, 2048, true);
    dirRecord(root, pos, "ABOUT.TXT;1", kHdLba + kDescriptor + 1, 10, false);
    dirRecord(root, pos, "DESC.BIN;1", kHdLba + kDescriptor, 0x100, false);
    dirRecord(root, pos, "IDTEST.BIN;1", kHdLba + kImage, kImageSize, false);

    uint8_t* desc = &hd[kDescriptor * 2048];
    memcpy(desc + 0xc0, imageName, strlen(imageName));
    memcpy(&hd[(kDescriptor + 1) * 2048], "not a game", 10);
    memcpy(&hd[kImage * 2048], enc.data(), enc.size());

    // --- PIC (16 KB dump layout) -----------------------------------------
    std::vector<uint8_t> pic(0x4000, 0);
    const char* picName = opt.useDescriptor ? "DESC.BIN" : imageName;
    char name14[14] = {};
    memcpy(name14, picName, strlen(picName));
    for (int i = 0; i < 7; i++)
    {
        pic[0x7c0 + i * 2] = (uint8_t)name14[i];
        pic[0x7e0 + i * 2] = (uint8_t)name14[i + 7];
        pic[0x780 + i * 2] = (uint8_t)(opt.picKey >> (56 - i * 8));
    }
    pic[0x7a0] = (uint8_t)opt.picKey;

    // --- CHD v5, uncompressed --------------------------------------------
    const uint32_t totalFrames = kHdLba + hdSectors;   // tracks 1+2 = 45000 frames
    const uint32_t hunkCount = totalFrames / kFramesPerHunk;
    const uint64_t logical = (uint64_t)totalFrames * kFrameBytes;

    const char* tracks[3][2] = {{"MODE1_RAW", "MODE1"}, {"AUDIO", "AUDIO"}, {"MODE1_RAW", "MODE1"}};
    const uint32_t frames[3] = {kTrack1Frames, kTrack2Frames, hdSectors};
    std::vector<std::vector<uint8_t>> meta(3);
    for (int t = 0; t < 3; t++)
    {
        char text[256];
        snprintf(text, sizeof(text),
                 "TRACK:%d TYPE:%s SUBTYPE:NONE FRAMES:%u PAD:0 PREGAP:0 PGTYPE:%s PGSUB:NONE POSTGAP:0", t + 1,
                 tracks[t][0], frames[t], tracks[t][1]);
        meta[t].assign(text, text + strlen(text) + 1);
    }

    const uint64_t mapOffset = 124;
    const uint64_t metaOffset = mapOffset + (uint64_t)hunkCount * 4;
    uint64_t end = metaOffset;
    for (int t = 0; t < 3; t++)
        end += 16 + meta[t].size();
    const uint32_t firstBlock = (uint32_t)((end + kHunkBytes - 1) / kHunkBytes);
    const uint32_t hdHunks = hdSectors / kFramesPerHunk;
    std::vector<uint8_t> chd((size_t)(firstBlock + hdHunks) * kHunkBytes, 0);

    uint8_t* h = chd.data();
    memcpy(h, "MComprHD", 8);
    be32(h + 8, 124);
    be32(h + 12, 5);
    be64(h + 32, logical);
    be64(h + 40, mapOffset);
    be64(h + 48, metaOffset);
    be32(h + 56, kHunkBytes);
    be32(h + 60, kFrameBytes);

    // Map: tracks 1 and 2 are all-zero hunks (entry 0), track 3 is stored.
    const uint32_t firstHdHunk = kHdLba / kFramesPerHunk;
    for (uint32_t i = 0; i < hdHunks; i++)
        be32(h + mapOffset + (uint64_t)(firstHdHunk + i) * 4, firstBlock + i);

    uint64_t m = metaOffset;
    for (int t = 0; t < 3; t++)
    {
        const uint64_t next = t < 2 ? m + 16 + meta[t].size() : 0;
        be32(h + m, ('C' << 24) | ('H' << 16) | ('G' << 8) | 'D');
        be32(h + m + 4, (1u << 24) | (uint32_t)meta[t].size());
        be64(h + m + 8, next);
        memcpy(h + m + 16, meta[t].data(), meta[t].size());
        m += 16 + meta[t].size();
    }

    for (uint32_t s = 0; s < hdSectors; s++)
    {
        uint8_t* frame = h + (size_t)firstBlock * kHunkBytes + (size_t)s * kFrameBytes;
        makeFrame(frame, 45150 + s, &hd[(size_t)s * 2048]);
    }

    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\')
        base += '/';
    out.chdPath = base + "gds-test.chd";
    out.picPath = base + "317-0384-com.pic";
    if (!writeFile(out.chdPath, chd) || !writeFile(out.picPath, pic))
    {
        error = "cannot write fixture files in " + dir;
        return false;
    }
    return true;
}

} // namespace fixture
