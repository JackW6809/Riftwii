// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The menu's background music: an OGG Vorbis file on the SD card,
// sd:/riftwii/music.ogg (the player's own) or else
// sd:/apps/riftwii/music.ogg (the one the release ships), looped while
// the menu is open and on through a launch's log, until the launch has to
// give the hardware up. menu_music = off in settings.txt (the Settings row)
// keeps it quiet.
namespace riftwii::wii {

// Starts it when the setting is on and a file is there. The file is read
// once, into MEM2, and kept for the menu's life.
void MenuMusicStart();
// Stops it; before audio is shut down and before any launch.
void MenuMusicStop();
// Whether a music file was found (the Settings row's note says so).
bool MenuMusicFound();

// Keeps the music playing past the menu, under the launch screen and its
// log, instead of stopping it with the menu. Returns false when nothing
// is playing; the caller then shuts the audio down itself, as before.
bool MenuMusicKeepForLaunch();
// Fades the kept music out over half a second, stops its decoder and shuts
// the audio down, saying `why` in the log. Called before the game's IOS
// is loaded or the game is loaded over RiftWii, and when a launch fails.
// Does nothing when no music was kept.
void LaunchMusicEnd(const char* why);
// Says in the log, with how far into the track it is, whether the kept
// music is still playing `when` (a cIOS reload, which it plays across).
void LaunchMusicNote(const char* when);

}  // namespace riftwii::wii
