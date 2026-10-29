// Run log. On the Wii nothing printed during a crash survives on screen, so
// every step of the boot/check flow is also appended to a file on the same
// storage device as the app (flushed per line: the next thing that happens
// may be a hang or a power cycle).
#pragma once

namespace idas3 {
namespace log {

// Truncates and opens the log (on the Wii the previous one becomes
// <name>_prev.txt). A failure is silent: the console still shows everything,
// there is just no file to send in.
void open(const char* path);
void close();

// printf-style, to the log file; on the Wii also to Dolphin's log window
// (libogc's OSReport UART), and to stdout when echo is on.
void write(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Also print every line on stdout. Off while the launcher draws its menus
// with the console; the game runner turns it on.
void setEcho(bool echo);

// While the game runs (Wii): lines go to a RAM ring that a writer thread
// takes to the card, synced every 500 ms, so the emulation never waits on
// the card. Off: the ring is written out and synced, lines go straight to
// the card again. Also turned off by close(), exit() and the quit sequence.
void setAsync(bool on);

// Raw text from stdout while the game runs (platform::captureStdout): the
// emulator core's own printf diagnostics. Flushed per write, synced to the
// card every 500 ms at most (or queued, see setAsync).
void writeRaw(const char* text, unsigned len);

const char* path();

} // namespace log
} // namespace idas3
