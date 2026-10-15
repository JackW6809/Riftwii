// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "riftwii/launch.hpp"
#include "usbcatalog.hpp"

// GameCube games on the SD card, started in Wii mode by RiftWii's own
// loader (gcnative.hpp). Patch-only mods (gcpatch.hpp) are listed as the
// game they make. Packs are not supported for them yet.
namespace riftwii::wii {

struct GcImage {
    std::string path;  // sd:/games/.../game.iso
    std::string id;    // ID6 from its header
    std::string patch_source;  // set: `path` is a patch (gcpatch.hpp) to make the game from this original
};

// A GameCube image on the SD card in games/<folder>/ (.iso or .gcm) whose
// ID starts with `game`, or the image at `path` when given.
bool FindGcImage(const std::string& game, const std::string& path, GcImage& out);

// Every GameCube image in sd:/games/<folder>/ (.iso or .gcm), for the
// game list: opened (header read), never needing d2x.
void ScanGcImages(std::vector<ImageGame>& out);

// Starts the game (prepared from its patch first, when it is one).
// Refuses any packs. Returns only on failure.
bool LaunchGcGame(const GcImage& image, const std::vector<PackageChoices>& packages, std::string& error);

}  // namespace riftwii::wii
