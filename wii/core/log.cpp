// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.hpp"

#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace riftwii::wii {
namespace {
FILE* g_file = nullptr;
std::string g_path;
bool g_line_start = true;
u64 g_epoch = 0;  // time base at the first LogOpen: file lines carry ms since then
// Off while the GUI owns the screen: libwiigui's framebuffer is also the
// text console's, so every echoed line flashed over the menus.
bool g_echo = false;
int g_batch = 0;        // LogBatch scopes alive
bool g_unsynced = false;  // lines in libfat's cache, not yet on the card

// LogHoldScreen: the lines not printed, the newest kHeldBytes of them in a
// ring. Static rather than a growing string: the hold runs after the menu
// has closed, the phase where a tester's heap once broke, and the crash
// screen prints it when the crash may have been inside malloc.
constexpr std::size_t kHeldBytes = 4096;
bool g_hold = false;
char g_held[kHeldBytes];
std::size_t g_held_next = 0;  // where the next byte goes
bool g_held_wrapped = false;  // full: the oldest byte is at g_held_next
u64 g_held_last_at = 0;       // gettime() at the last text held
// Taken around what goes to the screen while the log is held or echoed:
// the launch watch (wii/launch/launchwatch.cpp) shows the held lines from
// its own thread, and they must not mix with a line being logged. Made by
// the first LogHoldScreen, on the main thread, before the watch starts.
mutex_t g_screen_lock = LWP_MUTEX_NULL;

class ScreenGuard {
public:
    ScreenGuard() : lock_(g_echo || g_hold ? g_screen_lock : LWP_MUTEX_NULL) {
        if (lock_ != LWP_MUTEX_NULL) LWP_MutexLock(lock_);
    }
    ~ScreenGuard() {
        if (lock_ != LWP_MUTEX_NULL) LWP_MutexUnlock(lock_);
    }
    ScreenGuard(const ScreenGuard&) = delete;
    ScreenGuard& operator=(const ScreenGuard&) = delete;

private:
    mutex_t lock_;
};

void HoldText(const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
        g_held[g_held_next] = *c;
        g_held_next = (g_held_next + 1) % kHeldBytes;
        if (g_held_next == 0) g_held_wrapped = true;
    }
    g_held_last_at = gettime();
}

// Writes the held bytes from `first` to `count` (0 = the oldest) to the
// console, in the ring's two pieces when it has wrapped.
void WriteHeld(std::size_t first, std::size_t count) {
    const std::size_t oldest = g_held_wrapped ? g_held_next : 0;
    while (first < count) {
        const std::size_t at = (oldest + first) % kHeldBytes;
        const std::size_t until_end = kHeldBytes - at;
        const std::size_t piece = count - first < until_end ? count - first : until_end;
        std::fwrite(g_held + at, 1, piece, stdout);
        first += piece;
    }
}

// Clears the console's window, prints the held lines and
// goes back to echoing. A ring that wrapped lost the start of its oldest
// line: printing starts after its first newline.
void PrintHeld() {
    // Echo on before the hold goes off: a logf deciding whether to take
    // the lock always sees one of them, and waits for these lines.
    g_echo = true;
    g_hold = false;
    const std::size_t count = g_held_wrapped ? kHeldBytes : g_held_next;
    std::size_t first = 0;
    if (g_held_wrapped) {
        const std::size_t oldest = g_held_next;
        while (first < count && g_held[(oldest + first) % kHeldBytes] != '\n') ++first;
        if (first < count) ++first;  // past the newline itself
    }
    std::printf("\x1b[2J\x1b[0;0H");
    WriteHeld(first, count);
    g_held_next = 0;
    g_held_wrapped = false;
}
}  // namespace

void LogEchoToScreen(bool on) {
    g_echo = on;
    if (on) g_hold = false;
}

void LogHoldScreen() {
    if (g_screen_lock == LWP_MUTEX_NULL) LWP_MutexInit(&g_screen_lock, false);
    g_echo = false;
    g_held_next = 0;
    g_held_wrapped = false;
    g_held_last_at = gettime();
    g_hold = true;
}

bool LogHolding() { return g_hold; }

u64 LogHeldLastAt() { return g_held_last_at; }

void LogShowHeld() {
    ScreenGuard screen;
    if (!g_hold) return;
    PrintHeld();
}

void LogShowHeldForCrash() {
    if (!g_hold) return;
    // The crashed thread may be the one holding the lock (inside a line
    // being printed): after one try the crash screen goes on without it.
    const bool locked = g_screen_lock != LWP_MUTEX_NULL && LWP_MutexTryLock(g_screen_lock) == 0;
    PrintHeld();
    if (locked) LWP_MutexUnlock(g_screen_lock);
}

void LogOpen(const char* sd_path, bool append) {
    LogClose();
    if (g_epoch == 0) g_epoch = gettime();
    g_path = sd_path;
    g_file = std::fopen(sd_path, append ? "a" : "w");
    g_line_start = true;
}

void LogReopen() {
    if (g_file || g_path.empty()) return;
    g_file = std::fopen(g_path.c_str(), "a");
    g_line_start = true;
}

void LogSync() {
    if (!g_file || !g_unsynced) return;
    fsync(fileno(g_file));
    g_unsynced = false;
}

LogBatch::LogBatch() { ++g_batch; }

LogBatch::~LogBatch() {
    if (--g_batch == 0) LogSync();
}

void LogClose() {
    if (g_file) std::fclose(g_file);
    g_unsynced = false;
    g_file = nullptr;
}

void logf(const char* format, ...) {
    va_list args;
    char text[1024];
    bool formatted = false;
    {
        // Only the screen's part under the lock: a card write that hangs
        // must not keep the launch watch from showing the held lines.
        ScreenGuard screen;
        if (g_echo) {
            va_start(args, format);
            std::vprintf(format, args);
            va_end(args);
        } else if (g_hold) {
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            HoldText(text);
            formatted = true;
        }
    }
    if (!g_file) return;
    if (!formatted) {
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
    }
    if (g_line_start) {
        const unsigned long ms = static_cast<unsigned long>(ticks_to_millisecs(gettime() - g_epoch));
        std::fprintf(g_file, "[%6lu.%03lu] ", ms / 1000, ms % 1000);
    }
    std::fputs(text, g_file);
    const std::size_t n = std::char_traits<char>::length(text);
    g_line_start = n > 0 && text[n - 1] == '\n';
    // fflush only reaches libfat's sector cache; fsync writes the
    // cache to the card. Without it a hang loses every line since
    // the last close, which on hardware is exactly the part needed.
    std::fflush(g_file);
    g_unsynced = true;
    if (g_batch == 0) LogSync();
}

}  // namespace riftwii::wii
