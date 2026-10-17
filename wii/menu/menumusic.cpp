// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "menumusic.hpp"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "audio.h"
#include "loadersettings.hpp"
#include "log.hpp"
#include "menutheme.hpp"
#include "oggplayer.h"
#include "skin.hpp"

namespace riftwii::wii {
namespace {

constexpr long kMaxMusicBytes = 6 * 1024 * 1024;
constexpr int kVolume = 110;  // of 255: under the clicks, not over them
// The fade at the end of a launch: half a second, in 25 steps of 20 ms,
// each too small a drop in volume to be heard as a step.
constexpr int kFadeSteps = 25;
constexpr int kFadeStepUs = 20000;

const char* const kMusicFiles[] = {"sd:/riftwii/music.ogg", "sd:/apps/riftwii/music.ogg"};

u8* g_music = nullptr;
long g_bytes = 0;
bool g_tried = false;
bool g_playing = false;
bool g_kept_for_launch = false;

// Reads the first music file there is into MEM2 (not the MEM1 heap,
// which packs and scans need).
void Load() {
    if (g_tried) return;
    g_tried = true;
    // The theme's music.ogg first (docs/THEMES.md), then the card's, then RiftWii's.
    const std::string theme_music = MenuThemeMusic();
    std::vector<const char*> paths;
    if (!theme_music.empty()) paths.push_back(theme_music.c_str());
    for (const char* path : kMusicFiles) paths.push_back(path);
    for (const char* path : paths) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0 || size > kMaxMusicBytes) {
            logf("Menu music: %s is %ld bytes; at most %ld are played\n", path, size, kMaxMusicBytes);
            std::fclose(f);
            continue;
        }
        u8* data = skin::Mem2Alloc(static_cast<std::size_t>(size));
        const bool read = data && std::fread(data, 1, static_cast<std::size_t>(size), f) == static_cast<std::size_t>(size);
        std::fclose(f);
        if (!read) {
            logf("Menu music: cannot read %s%s\n", path, data ? "" : " (MEM2 is full)");
            return;  // MEM2 taken is not given back; one try is enough
        }
        g_music = data;
        g_bytes = size;
        logf("Menu music: %s (%ld bytes)\n", path, size);
        return;
    }
}

}  // namespace

void MenuMusicStart() {
    if (g_playing || Settings().menu_music == "off") return;
    Load();
    if (!g_music) return;
    if (PlayOgg(g_music, g_bytes, 0, OGG_INFINITE_TIME) < 0) {
        logf("Menu music: the player refused the file\n");
        return;
    }
    SetVolumeOgg(kVolume);
    g_playing = true;
}

void MenuMusicStop() {
    g_kept_for_launch = false;
    if (!g_playing) return;
    StopOgg();
    g_playing = false;
}

bool MenuMusicFound() {
    Load();
    return g_music != nullptr;
}

bool MenuMusicKeepForLaunch() {
    if (!g_playing) return false;
    // The decoder runs on its own thread from the file in MEM2 and the
    // mixer on the audio interrupt, so neither needs the GUI thread, which
    // is halted from here on, nor the card, which the launch reads.
    g_kept_for_launch = true;
    return true;
}

void LaunchMusicEnd(const char* why) {
    if (!g_kept_for_launch) return;
    g_kept_for_launch = false;
    const u64 start = gettime();
    for (int step = kFadeSteps - 1; step >= 0; --step) {
        SetVolumeOgg(kVolume * step / kFadeSteps);
        usleep(kFadeStepUs);
    }
    // The decoder's thread is joined and Tremor's memory freed here; the
    // mixer goes with the audio, before anything the next IOS or the game
    // could find still running.
    MenuMusicStop();
    ShutdownAudio();
    logf("Menu music: faded out in %u ms (%s)\n",
         static_cast<unsigned>(ticks_to_millisecs(gettime() - start)), why);
}

void LaunchMusicNote(const char* when) {
    if (!g_kept_for_launch) return;
    // The track's position moves only while the decoder runs, so two notes
    // either side of a reload show whether it played across.
    const s32 at_ms = GetTimeOgg();
    const bool running = StatusOgg() == 1;
    logf("Menu music: %s %s, %d.%01d s into the track\n", running ? "playing" : "NOT playing", when,
         static_cast<int>(at_ms / 1000), static_cast<int>(at_ms % 1000 / 100));
}

}  // namespace riftwii::wii
