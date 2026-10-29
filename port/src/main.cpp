// Entry point for both builds.
//
//   (no arguments)                 the pre-launch menu
//   --dir <folder>                 Windows: folder the file browser opens in
//   --check <file.chd>             headless: run the game check and print it
//       [--load] [--plan full|noelan|ram]   ... then load the program
//       [--extract <out.bin>]      ... and/or write the decrypted DIMM image
//       [--dump <offset> <length>] ... and/or hexdump decrypted image bytes
#include "core/game_session.h"
#include "core/log.h"
#include "platform/platform.h"
#include "ui/launcher.h"
#include "version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace idas3;

static const char* argValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return nullptr;
}

static bool hasArg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

static int runHeadless(int argc, char** argv)
{
    const char* chd = argValue(argc, argv, "--check");
    log::open(argValue(argc, argv, "--log") ? argValue(argc, argv, "--log") : "");
    printf("%s %s (headless) on %s\n", IDAS3_PORT_NAME, IDAS3_PORT_VERSION, platform::name());

    static const char* tags[] = {"[ OK ]", "[WARN]", "[FAIL]", "[INFO]"};
    size_t printed = 0;
    auto progress = [&](const std::vector<CheckLine>& lines, const std::string&) {
        for (; printed < lines.size(); printed++)
            printf("%s %s\n", tags[(int)lines[printed].status], lines[printed].text.c_str());
        fflush(stdout);
    };

    GameSession session;
    if (!session.check(chd, progress))
        return 1;

    if (const char* d = argValue(argc, argv, "--dump"))
    {
        uint32_t offset = (uint32_t)strtoul(d, nullptr, 0);
        uint32_t length = 256;
        for (int i = 1; i + 2 < argc; i++)
            if (!strcmp(argv[i], "--dump"))
                length = (uint32_t)strtoul(argv[i + 2], nullptr, 0);
        std::vector<uint8_t> buf(length);
        if (!session.dimm().read(offset, length, buf.data()))
            return 1;
        for (uint32_t i = 0; i < length; i += 16)
        {
            printf("%08x ", offset + i);
            for (uint32_t j = 0; j < 16 && i + j < length; j++)
                printf(" %02x", buf[i + j]);
            printf("  ");
            for (uint32_t j = 0; j < 16 && i + j < length; j++)
                putchar(buf[i + j] >= 0x20 && buf[i + j] < 0x7f ? buf[i + j] : '.');
            putchar('\n');
        }
    }

    if (hasArg(argc, argv, "--load"))
    {
        MemoryPlan plan = MemoryPlan::Full;
        if (const char* p = argValue(argc, argv, "--plan"))
            plan = !strcmp(p, "ram") ? MemoryPlan::RamOnly : !strcmp(p, "noelan") ? MemoryPlan::NoElan : MemoryPlan::Full;
        if (!session.load(plan, progress))
            return 1;
    }

    if (const char* out = argValue(argc, argv, "--extract"))
    {
        std::string error;
        int lastPct = -1;
        const bool ok = session.dimm().extract(out, [&](uint32_t done, uint32_t total) {
            const int pct = total ? (int)((uint64_t)done * 100 / total) : 100;
            if (pct / 10 != lastPct / 10)
                printf("extract: %d%%\n", pct);
            lastPct = pct;
            return true;
        }, error);
        if (!ok)
        {
            printf("[FAIL] extract: %s\n", error.c_str());
            return 1;
        }
        printf("[ OK ] decrypted image written to %s\n", out);
    }
    session.close();
    log::close();
    return 0;
}

int main(int argc, char** argv)
{
    if (argValue(argc, argv, "--check"))
        return runHeadless(argc, argv);

    platform::init(argc, argv);
    log::open(platform::logPath().c_str());
    const int rc = ui::runLauncher();
    log::close();
    platform::shutdown();
    return rc;
}
