// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "riftwii/launch.hpp"
#include "usbcatalog.hpp"

// GameCube games, started through RiftWii's build of Nintendont
// (sd:/apps/nintendont-riftwii/boot.dol). Packs are applied the way the
// Wii games' are, without touching the image: the patched files are
// served by Nintendont's kernel from /riftwii/gc/overlay.bin
// (riftwii/gcoverlay.hpp). For now the game and its packs' files must be
// on the SD card, the one device Nintendont reads them from.
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

// Builds the overlay for the packs (in order, each with the choices
// picked over its defaults; none: the game as it is) and starts
// Nintendont. Returns only on failure.
bool LaunchGcGame(const GcImage& image, const std::vector<PackageChoices>& packages, std::string& error);

}  // namespace riftwii::wii
