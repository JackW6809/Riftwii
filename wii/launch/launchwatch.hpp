// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The launch watch: while a quiet launch screen holds the log back
// (LogHoldScreen), a small thread of its own shows the held lines when the
// player presses B (or 1) on a Wii Remote or B on a GameCube controller,
// and by itself once neither the bar nor the log has moved for 15 seconds.
// The launch itself never polls the controllers, and a hang is mostly the
// main thread waiting inside an IOS call, where only another thread can
// still look. A normal launch shows nothing of it.
namespace riftwii::wii {

// After LogHoldScreen, on the main thread. Does nothing when the log is
// not held.
void LaunchWatchStart();
// Stops the thread and waits for it: before the game is handed the
// machine (libogc shuts down), and before anything else reads the
// controllers (the offer after a failed launch). Safe to call twice.
void LaunchWatchStop();
// The Wii Remotes go away (WPAD_Shutdown, an IOS reload): from here only
// GameCube controllers are read. Waits for a read in progress to finish.
void LaunchWatchRemotesGone();

}  // namespace riftwii::wii
