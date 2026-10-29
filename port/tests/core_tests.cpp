// Core pipeline tests. No game data: a synthetic GD-ROM CHD + PIC is built
// in the folder given as argv[1].
#include "fixture.h"
#include "core/bytes.h"
#include "core/dimm_image.h"
#include "core/game_session.h"
#include "core/naomi_boot.h"
#include "core/naomi_des.h"
#include "core/naomi_pic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace idas3;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        g_checks++;                                                              \
        if (!(cond))                                                             \
        {                                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void testBytes()
{
    printf("bytes\n");
    uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    CHECK(rd_le16(b) == 0x0201);
    CHECK(rd_le32(b) == 0x04030201);
    CHECK(rd_le64(b) == 0x0807060504030201ULL);
    wr_le64(b, 0x1122334455667788ULL);
    CHECK(b[0] == 0x88 && b[7] == 0x11);
}

static void testDesKnownAnswer()
{
    printf("des: FIPS known-answer vector\n");
    // Textbook DES: K=133457799BBCDFF1, P=0123456789ABCDEF -> C=85E813540F0AB405.
    // NaomiDes::setKey byte-reverses the PIC key (MAME's rev64), so the PIC
    // key for K is F1DFBC9B79573413.
    NaomiDes des;
    des.setKey(0xF1DFBC9B79573413ULL);
    CHECK(des.encryptBlock(0x0123456789ABCDEFULL) == 0x85E813540F0AB405ULL);
    CHECK(des.decryptBlock(0x85E813540F0AB405ULL) == 0x0123456789ABCDEFULL);

    // Buffers are little-endian u64 blocks on every host.
    uint8_t buf[16];
    wr_le64(buf, 0x85E813540F0AB405ULL);
    wr_le64(buf + 8, 0x85E813540F0AB405ULL);
    des.decryptBuffer(buf, sizeof(buf));
    CHECK(rd_le64(buf) == 0x0123456789ABCDEFULL);
    CHECK(rd_le64(buf + 8) == 0x0123456789ABCDEFULL);
    CHECK(buf[0] == 0xEF && buf[7] == 0x01);

    // Round trip with an arbitrary key.
    des.setKey(0x0011223344556677ULL);
    for (uint64_t v = 1; v < 0xffffffffffULL; v = v * 3 + 0x12345)
        CHECK(des.decryptBlock(des.encryptBlock(v)) == v);
}

static void testPic()
{
    printf("pic\n");
    std::vector<uint8_t> pic(0x4000, 0);
    const char* name = "ABCDEFGHIJKLM";
    for (int i = 0; i < 7; i++)
    {
        pic[0x7c0 + i * 2] = (uint8_t)name[i];
        pic[0x7e0 + i * 2] = (uint8_t)name[i + 7];   // name[13] is the terminator
        pic[0x780 + i * 2] = (uint8_t)(0xA0 + i);
    }
    pic[0x7a0] = 0x5A;
    NaomiPic p;
    std::string err;
    CHECK(parseNaomiPic(pic, p, err));
    CHECK(p.fileName == "ABCDEFGHIJKLM");
    CHECK(p.desKey == 0xA0A1A2A3A4A5A65AULL);
    CHECK(p.fullDump && !p.netPic);

    std::vector<uint8_t> tooSmall(16, 0);
    CHECK(!parseNaomiPic(tooSmall, p, err));
}

static void testIsoHelpers()
{
    printf("iso9660 helpers\n");
    CHECK(isoNameMatches("IDTEST.BIN", "IDTEST.BIN"));
    CHECK(isoNameMatches("IDTEST.BIN", "*.BIN"));
    CHECK(!isoNameMatches("ABOUT.TXT", "*.BIN"));
    CHECK(!isoNameMatches("IDTEST.BIN", "IDTEST"));
}

static bool fileEquals(const std::string& path, const std::vector<uint8_t>& expected)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::vector<uint8_t> data(expected.size() + 1);
    const size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    return n == expected.size() && memcmp(data.data(), expected.data(), n) == 0;
}

static void testEndToEnd(const std::string& dir, bool useDescriptor)
{
    printf("end to end (%s)\n", useDescriptor ? "descriptor indirection" : "direct file name");
    fixture::Options opt;
    opt.useDescriptor = useDescriptor;
    fixture::Fixture fx;
    std::string error;
    CHECK(fixture::build(dir, opt, fx, error));

    GameSession session;
    const bool checked = session.check(fx.chdPath, nullptr);
    CHECK(checked);
    for (const CheckLine& l : session.lines())
        printf("    %d %s\n", (int)l.status, l.text.c_str());
    if (!checked)
        return;

    const NaomiBootHeader& h = session.header();
    CHECK(h.isNaomi2());
    CHECK(h.displayTitle() == "INITIAL D TEST EXPORT");
    CHECK(h.year == 2004 && h.month == 2 && h.day == 29);
    CHECK(h.gameLoad.size() == 2);
    CHECK(h.gamePC == fx.gamePC);
    CHECK(session.isSupportedGame());
    CHECK(session.picPath() == fx.picPath);
    CHECK(session.dimm().size() == fx.plainImage.size());

    // Random-access decrypted reads, across sector and run boundaries and
    // past the end of the image.
    const uint32_t size = (uint32_t)fx.plainImage.size();
    const uint32_t probes[][2] = {
        {0, 16}, {2040, 20}, {2048 * 8 - 3, 9}, {12345, 70000}, {size - 100, 100}, {size - 10, 50}, {7, 1},
    };
    for (const auto& p : probes)
    {
        std::vector<uint8_t> got(p[1]);
        CHECK(session.dimm().read(p[0], p[1], got.data()));
        for (uint32_t i = 0; i < p[1]; i++)
        {
            const uint8_t want = p[0] + i < size ? fx.plainImage[p[0] + i] : 0;
            if (got[i] != want)
            {
                CHECK(got[i] == want);
                break;
            }
        }
    }

    CHECK(session.load(MemoryPlan::RamOnly, nullptr));
    CHECK(session.loaded());
    CHECK(session.loadedBytes() == 0x8000 + 0x1234);
    const uint8_t* seg0 = session.memory().ramPointer(0x0c020000, 0x8000);
    const uint8_t* seg1 = session.memory().ramPointer(0x0c030000, 0x1234);   // loaded via the P1 mirror
    CHECK(seg0 && memcmp(seg0, &fx.plainImage[0x1000], 0x8000) == 0);
    CHECK(seg1 && memcmp(seg1, &fx.plainImage[0x9000], 0x1234) == 0);
    const uint8_t* hdr = session.memory().ramPointer(kNaomiBootHeaderRamAddress, kNaomiBootHeaderSize);
    CHECK(hdr && memcmp(hdr, fx.plainImage.data(), kNaomiBootHeaderSize) == 0);
    CHECK(session.memory().ramPointer(0x0c000000 + kNaomiRamSize - 4, 8) == nullptr);
    CHECK(session.memory().ramPointer(0x04000000, 4) == nullptr);

    const std::string extracted = dir + "/extracted.bin";
    CHECK(session.dimm().extract(extracted.c_str(), nullptr, error));
    CHECK(fileEquals(extracted, fx.plainImage));
    std::remove(extracted.c_str());
    session.close();
}

static void testWrongKey(const std::string& dir)
{
    printf("wrong PIC key\n");
    fixture::Options opt;
    opt.encryptKey = 0x1111111111111111ULL;   // the PIC still holds the default key
    fixture::Fixture fx;
    std::string error;
    CHECK(fixture::build(dir, opt, fx, error));
    GameSession session;
    CHECK(!session.check(fx.chdPath, nullptr));
    CHECK(!session.lines().empty() && session.lines().back().status == CheckStatus::Fail);
    CHECK(!session.lines().empty() && session.lines().back().text.find("wrong PIC") != std::string::npos);
}

static void testMissingPic(const std::string& dir)
{
    printf("missing PIC\n");
    fixture::Fixture fx;
    std::string error;
    CHECK(fixture::build(dir, fixture::Options(), fx, error));
    std::remove(fx.picPath.c_str());
    GameSession session;
    CHECK(!session.check(fx.chdPath, nullptr));
    CHECK(!session.lines().empty() && session.lines().back().text.find("PIC not found") != std::string::npos);
}

static void testNotAChd(const std::string& dir)
{
    printf("not a CHD\n");
    const std::string path = dir + "/bogus.chd";
    FILE* f = fopen(path.c_str(), "wb");
    if (f)
    {
        fputs("this is not a CHD file at all", f);
        fclose(f);
    }
    GameSession session;
    CHECK(!session.check(path, nullptr));
    CHECK(!session.lines().empty() && session.lines().back().status == CheckStatus::Fail);
    std::remove(path.c_str());
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "test_fixture";
    for (const char* sub : {"direct", "descriptor", "wrongkey", "nopic"})
        std::filesystem::create_directories(dir + "/" + sub);

    testBytes();
    testDesKnownAnswer();
    testPic();
    testIsoHelpers();
    testEndToEnd(dir + "/direct", false);
    testEndToEnd(dir + "/descriptor", true);
    testWrongKey(dir + "/wrongkey");
    testMissingPic(dir + "/nopic");
    testNotAChd(dir);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
