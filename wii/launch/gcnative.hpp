// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gclaunch.hpp"
#include "riftwii/gcoverlay.hpp"

namespace riftwii::wii {

// Starts a GameCube game in Wii mode with RiftWii's own code: the game is
// loaded from its image on the SD card, its DVDLow* functions are replaced
// by runtime/gc (served from the card's sectors), and RiftWii jumps to it.
// No disc drive. `pack`, when given, is the packs' plan: their files are
// served from its window past the image's end (riftwii/gcoverlay.hpp).
// Returns only on failure.
bool LaunchGcNative(const GcImage& image, const GcOverlayPlan* pack, std::string& error);

// Where holding HOME in a GameCube game goes, as Settings > Return to
// says: the RiftWii channel (on "riftwii", when it is installed), else the
// Wii Menu; None when neither has a ticket. (HomebrewChannel is never
// chosen any more; the value stays for the menu's crash texts.)
enum class GcWayBack { None, Channel, HomebrewChannel, WiiMenu };
GcWayBack FindGcWayBack();

// The crash screen's message, made by the menu in its own font and
// language (8-bit coverage, kGcCrashCardW x kGcCrashCardH): the next
// GameCube launch shows it if the game crashes. Without one a crash turns
// the screen white.
constexpr int kGcCrashCardW = 512, kGcCrashCardH = 128;
void SetGcCrashCard(std::vector<std::uint8_t> coverage);

}  // namespace riftwii::wii
