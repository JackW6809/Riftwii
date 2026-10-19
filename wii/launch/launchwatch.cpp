// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launchwatch.hpp"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/pad.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#include "log.hpp"
#include "progress.hpp"

namespace riftwii::wii {
namespace {

// Long enough for the slowest step that still moves now and then (a big
// game image opened over USB, the cIOS search), short enough that a
// player looking at a frozen bar is not left guessing.
constexpr u64 kStillSeconds = 15;
constexpr useconds_t kTickMicroseconds = 50000;

// In the DOL's own memory, not the heap: the launch overwrites most of
// MEM1 around it, and the heap is what a tester's launch once broke.
alignas(32) u8 g_stack[8192];
lwp_t g_thread = LWP_THREAD_NULL;
volatile bool g_running = false;
volatile bool g_finished = false;  // the thread has left Watch
volatile bool g_remotes = false;
// Held while the thread reads the Wii Remotes, so LaunchWatchRemotesGone
// can wait for a read to finish before WPAD shuts down.
mutex_t g_input_lock = LWP_MUTEX_NULL;
u64 g_started_at = 0;

bool RevealPressed() {
    bool pressed = false;
    // The GameCube ports are the SI's, which no IOS reload touches.
    PAD_ScanPads();
    for (int pad = 0; pad < 4; ++pad) {
        if (PAD_ButtonsDown(pad) & PAD_BUTTON_B) pressed = true;
    }
    LWP_MutexLock(g_input_lock);
    if (g_remotes) {
        WPAD_ScanPads();
        for (int remote = 0; remote < 4; ++remote) {
            const u32 down = WPAD_ButtonsDown(remote);
            if (down & (WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_B)) pressed = true;
        }
    }
    LWP_MutexUnlock(g_input_lock);
    return pressed;
}

// The last sign of life: a new stage or step of the bar, or a log line.
// A line counts too, so a long step that reports as it goes (the cIOS
// search tries its slots one by one) is not taken for a stuck one.
u64 LastMovedAt() {
    u64 last = g_started_at;
    const u64 progress = ProgressMovedAt();
    const u64 line = LogHeldLastAt();
    if (progress > last) last = progress;
    if (line > last) last = line;
    return last;
}

void* Watch(void*) {
    while (g_running && LogHolding()) {
        usleep(kTickMicroseconds);
        if (!g_running || !LogHolding()) break;
        // Shown first, then said, on the screen and in boot.log: when the
        // launch hangs in a card write, the line waits but the screen does not.
        if (RevealPressed()) {
            LogShowHeld();
            logf("Launch screen: the log is shown because a button was pressed; the launch goes on\n");
            break;
        }
        if (ticks_to_secs(gettime() - LastMovedAt()) >= kStillSeconds) {
            LogShowHeld();
            logf("Launch screen: the log is shown because nothing has moved for %u seconds; the launch may be "
                 "stuck\n", static_cast<unsigned>(kStillSeconds));
            break;
        }
    }
    g_finished = true;
    return nullptr;
}

}  // namespace

void LaunchWatchStart() {
    if (g_thread != LWP_THREAD_NULL || !LogHolding()) return;
    if (g_input_lock == LWP_MUTEX_NULL) LWP_MutexInit(&g_input_lock, false);
    g_started_at = gettime();
    g_remotes = true;
    g_running = true;
    g_finished = false;
    // Above the main thread, so a launch spinning on a wait (the IOS
    // reload's) still lets it look; it sleeps between looks.
    if (LWP_CreateThread(&g_thread, Watch, nullptr, g_stack, sizeof(g_stack), 80) < 0) {
        g_thread = LWP_THREAD_NULL;
        g_running = false;
        logf("Launch screen: the watch for a stuck launch did not start; the log shows only if the launch fails\n");
    }
}

void LaunchWatchStop() {
    if (g_thread == LWP_THREAD_NULL) return;
    g_running = false;
    // Not LWP_JoinThread: after a crash the thread may be waiting for the
    // screen's lock, which the crashed thread holds. A thread still busy
    // after a second is parked instead, so it never runs again.
    if (LWP_GetSelf() != g_thread) {
        const u64 asked_at = gettime();
        while (!g_finished && ticks_to_millisecs(gettime() - asked_at) < 1000) usleep(10000);
        if (!g_finished) LWP_SuspendThread(g_thread);
    }
    g_thread = LWP_THREAD_NULL;
}

void LaunchWatchRemotesGone() {
    if (g_input_lock == LWP_MUTEX_NULL) {
        g_remotes = false;
        return;
    }
    LWP_MutexLock(g_input_lock);
    g_remotes = false;
    LWP_MutexUnlock(g_input_lock);
}

}  // namespace riftwii::wii
