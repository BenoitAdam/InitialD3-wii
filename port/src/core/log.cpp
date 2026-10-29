#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(GEKKO)
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <unistd.h>
#endif

namespace idas3 {
namespace log {

static FILE* s_file = nullptr;
static char s_path[512] = "";
static bool s_echo = false;

#if defined(GEKKO)
// Asynchronous mode (while the game runs): text goes to a RAM ring and a
// writer thread moves it to the card. A card sync stalls the caller for
// ~10 ms per line on hardware (log 8: the profiler's 120-frame block cost a
// 40-60 ms frame, the 500 ms syncs of the core printf ~3 more a second); the
// writer, above the emulation's priority, only runs for the copy and sleeps
// in IOS while the card works. A crash loses what is not synced yet (half a
// second at most, as the core printf already did).
constexpr u32 kRingBytes = 32 * 1024;           // power of two; ~5 KB/s with the profiler on
constexpr u32 kWriterPrio = 80;                 // main thread: 64
constexpr u32 kWriterSleepUs = 40 * 1000;
constexpr u32 kSyncMs = 500;
static char* s_ring = nullptr;
static volatile u32 s_head = 0, s_tail = 0;     // bytes put / written, free-running
static volatile u32 s_dropped = 0;              // bytes that found the ring full
static volatile bool s_async = false, s_stop = false;
static lwp_t s_writer = LWP_THREAD_NULL;

static void ringPut(const char* text, u32 len)
{
    const u32 level = IRQ_Disable();
    if (len > kRingBytes - (s_head - s_tail))
        s_dropped += len;
    else
    {
        const u32 at = s_head & (kRingBytes - 1);
        const u32 first = len < kRingBytes - at ? len : kRingBytes - at;
        memcpy(s_ring + at, text, first);
        memcpy(s_ring, text + first, len - first);
        __asm__ volatile("" ::: "memory");      // the text before the head that publishes it
        s_head += len;
    }
    IRQ_Restore(level);
}

// Writes the ring out up to the head seen on entry. True when it wrote.
static bool ringDrain()
{
    const u32 head = s_head;
    u32 tail = s_tail;
    if (tail == head && !s_dropped)
        return false;
    while (tail != head)
    {
        const u32 at = tail & (kRingBytes - 1);
        const u32 n = head - tail < kRingBytes - at ? head - tail : kRingBytes - at;
        fwrite(s_ring + at, 1, n, s_file);
        tail += n;
        s_tail = tail;                          // frees the space
    }
    if (s_dropped)
    {
        const u32 level = IRQ_Disable();
        const u32 dropped = s_dropped;
        s_dropped = 0;
        IRQ_Restore(level);
        fprintf(s_file, "[log] %u bytes lost: the card did not keep up\n", dropped);
    }
    fflush(s_file);
    return true;
}

static void* writerMain(void*)
{
    u64 synced = gettime();
    bool unsynced = false;
    for (;;)
    {
        const bool stop = s_stop;
        if (ringDrain())
            unsynced = true;
        const u64 now = gettime();
        if (unsynced && (stop || ticks_to_millisecs(diff_ticks(synced, now)) >= kSyncMs))
        {
            fsync(fileno(s_file));
            synced = now;
            unsynced = false;
        }
        if (stop)
            return nullptr;
        usleep(kWriterSleepUs);
    }
}

static void drainAtExit()
{
    setAsync(false);
}
#endif

void open(const char* p)
{
    close();
    snprintf(s_path, sizeof(s_path), "%s", p ? p : "");
    if (!s_path[0])
        return;
#if defined(GEKKO)
    // The previous run's log is kept next to it (..._prev.txt): after a crash
    // the Wii is usually relaunched before the card is read.
    char prev[sizeof(s_path) + 8];
    const size_t n = strlen(s_path);
    if (n > 4 && !strcmp(s_path + n - 4, ".txt"))
        snprintf(prev, sizeof(prev), "%.*s_prev.txt", (int)(n - 4), s_path);
    else
        snprintf(prev, sizeof(prev), "%s.prev", s_path);
    remove(prev);
    rename(s_path, prev);
#endif
    s_file = fopen(s_path, "w");
}

void close()
{
    setAsync(false);
    if (s_file)
    {
        fclose(s_file);
        s_file = nullptr;
    }
}

void setEcho(bool echo)
{
    s_echo = echo;
}

void setAsync(bool on)
{
#if defined(GEKKO)
    if (on)
    {
        if (s_async || !s_file)
            return;
        if (!s_ring)
            s_ring = (char*)malloc(kRingBytes);
        if (!s_ring)
            return;                             // stays synchronous
        s_head = s_tail = s_dropped = 0;
        s_stop = false;
        s_async = true;
        if (LWP_CreateThread(&s_writer, writerMain, nullptr, nullptr, 16 * 1024, kWriterPrio) < 0)
        {
            s_writer = LWP_THREAD_NULL;
            s_async = false;
            return;
        }
        static bool s_atExit = false;
        if (!s_atExit)
        {
            atexit(drainAtExit);                // exit() from the quit combo
            s_atExit = true;
        }
    }
    else if (s_async)
    {
        s_stop = true;
        LWP_JoinThread(s_writer, nullptr);      // drains and syncs
        s_writer = LWP_THREAD_NULL;
        const u32 level = IRQ_Disable();
        s_async = false;                        // from here on, straight to the card
        IRQ_Restore(level);
        if (ringDrain())                        // put after the writer's last look
            fsync(fileno(s_file));
    }
#else
    (void)on;
#endif
}

void write(const char* fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

#if defined(GEKKO)
    if (s_async)
    {
        ringPut(text, (u32)strlen(text));
        SYS_Report("%s", text);
        return;
    }
#endif
    if (s_file)
    {
        fputs(text, s_file);
        fflush(s_file);
#if defined(GEKKO)
        // Through libfat's cache to the card: a crash must not lose the trail.
        fsync(fileno(s_file));
#endif
    }
#if defined(GEKKO)
    // Dolphin's log window (View > Show Log, "OSReport EXI" enabled).
    SYS_Report("%s", text);
#endif
    if (s_echo)
        fputs(text, stdout);
}

void writeRaw(const char* text, unsigned len)
{
    if (!len)
        return;
#if defined(GEKKO)
    if (s_async)
        ringPut(text, len);
    else
#endif
    if (s_file)
    {
        fwrite(text, 1, len, s_file);
        fflush(s_file);
#if defined(GEKKO)
        // To the card twice a second at most (a sync per line would cost a
        // few percent with the profiler on): a crash loses half a second.
        static u64 s_synced = 0;
        const u64 now = gettime();
        if (ticks_to_millisecs(diff_ticks(s_synced, now)) >= 500)
        {
            fsync(fileno(s_file));
            s_synced = now;
        }
#endif
    }
#if defined(GEKKO)
    char line[512];
    const unsigned n = len < sizeof(line) - 1 ? len : (unsigned)sizeof(line) - 1;
    memcpy(line, text, n);
    line[n] = 0;
    SYS_Report("%s", line);
#endif
}

const char* path()
{
    return s_path;
}

} // namespace log
} // namespace idas3

#if defined(GEKKO)
// The core's quit sequence (wii/wii_exit.cpp) calls this, weakly linked,
// before it starts tearing devices down: every line of it reaches the card.
extern "C" void wii_log_sync(void)
{
    idas3::log::setAsync(false);
}
#endif
