// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "gclaunch.hpp"
#include "usbcatalog.hpp"

namespace riftwii::wii {

// Patch-only GameCube mods: VCDIFF (xdelta) patches in sd:/riftwii/patches.
// A patch is listed as the game it makes when one of the GameCube images
// found is its original (its first window's checksum says so) and that
// game is not there already. Starting it prepares the image once, into
// sd:/games/<ID>/game.iso, from then on an ordinary game.
constexpr const char* kGcPatchDir = "sd:/riftwii/patches";

// Adds an entry (gc_patch_source set) for each patch whose original is
// among `games` and whose result is not.
void ScanGcPatches(std::vector<ImageGame>& games);

// Makes the patched image `image` (patch in path, original in
// patch_source) as sd:/games/<ID>/game.iso and points `image` at it.
bool PrepareGcPatch(GcImage& image, std::string& error);

}  // namespace riftwii::wii
