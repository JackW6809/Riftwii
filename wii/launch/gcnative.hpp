// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

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

}  // namespace riftwii::wii
