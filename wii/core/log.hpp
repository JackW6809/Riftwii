// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gctypes.h>

// printf-style logging for the console/boot path: goes to the on-screen
// console (and the USB Gecko when one is attached, see ConsoleStart) and,
// while a log file is open, to the SD card as well.
namespace riftwii::wii {

void LogOpen(const char* sd_path, bool append = false);
void LogClose();
// Opens the last path again, appending: for code that must close the
// file around an unmount of the card.
void LogReopen();
void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Whether logf also prints to the text console. ConsoleStart turns it on;
// it is off during the GUI, where lines go to the SD log only.
void LogEchoToScreen(bool on);
// Every line is written to the card as it is logged (about 9 ms each on a
// Wii's SD card), so a hang leaves the line before it. Lines that only
// report, with nothing between them that could hang, are held back while a
// LogBatch lives and reach the card together when the last one ends.
class LogBatch {
public:
    LogBatch();
    ~LogBatch();
    LogBatch(const LogBatch&) = delete;
    LogBatch& operator=(const LogBatch&) = delete;
};
// Writes what a LogBatch held back to the card now: before a step inside
// the batch that could hang.
void LogSync();
// The launch screen without its log (a tester: "we don't need to see the
// code"): lines are kept, their last 4 KB in a static ring, instead of
// printed, until LogShowHeld clears the console's window, prints them and
// goes back to echoing: a failed launch, or the launch watch
// (wii/launch/launchwatch.hpp) for one that seems stuck, shows what
// happened. Safe to call from another thread.
void LogHoldScreen();
void LogShowHeld();
// The same for the crash screen: does not wait for a line being printed
// (the crash may be inside it) and takes no heap.
void LogShowHeldForCrash();
// Whether lines are being held, and gettime() at the last one held (or at
// LogHoldScreen): the launch watch's sign of life.
bool LogHolding();
u64 LogHeldLastAt();

}  // namespace riftwii::wii
