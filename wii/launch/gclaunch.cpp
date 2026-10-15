// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gclaunch.hpp"

#include <dirent.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "gcnative.hpp"
#include "gcpatch.hpp"
#include "log.hpp"

namespace riftwii::wii {
namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

bool ends_with(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}

// The first 0x20 bytes say what the image is: its ID and the GameCube magic.
bool read_gc_id(const std::string& path, std::string& id) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::uint8_t head[0x20];
    const bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head);
    std::fclose(f);
    if (!ok || be32(head + 0x1C) != 0xC2339F3Du) return false;
    id.assign(reinterpret_cast<const char*>(head), 6);
    return true;
}

}  // namespace

bool FindGcImage(const std::string& game, const std::string& path, GcImage& out) {
    if (!path.empty()) {
        std::string id;
        if (!read_gc_id(path, id) || id.compare(0, game.size(), game) != 0) return false;
        out.path = path;
        out.id = id;
        return true;
    }
    DIR* games = opendir("sd:/games");
    if (!games) return false;
    bool found = false;
    while (dirent* folder = readdir(games)) {
        if (folder->d_name[0] == '.') continue;
        const std::string dir = std::string("sd:/games/") + folder->d_name;
        DIR* inner = opendir(dir.c_str());
        if (!inner) continue;
        while (dirent* file = readdir(inner)) {
            const std::string name = file->d_name;
            if (name[0] == '.' || !(ends_with(name, ".iso") || ends_with(name, ".gcm"))) continue;
            std::string id;
            const std::string full = dir + "/" + name;
            if (read_gc_id(full, id) && id.compare(0, game.size(), game) == 0) {
                out.path = full;
                out.id = id;
                found = true;
                break;
            }
        }
        closedir(inner);
        if (found) break;
    }
    closedir(games);
    if (found) return true;
    // A patch that makes it from one of them.
    std::vector<ImageGame> all;
    ScanGcImages(all);
    for (const ImageGame& g : all) {
        if (g.gc_patch_source.empty() || g.id.compare(0, game.size(), game) != 0) continue;
        out.path = g.path;
        out.id = g.id;
        out.patch_source = g.gc_patch_source;
        return true;
    }
    return false;
}

void ScanGcImages(std::vector<ImageGame>& out) {
    DIR* games = opendir("sd:/games");
    if (!games) return;
    while (dirent* folder = readdir(games)) {
        if (folder->d_name[0] == '.') continue;
        const std::string dir = std::string("sd:/games/") + folder->d_name;
        DIR* inner = opendir(dir.c_str());
        if (!inner) continue;
        while (dirent* file = readdir(inner)) {
            const std::string name = file->d_name;
            if (name[0] == '.' || !(ends_with(name, ".iso") || ends_with(name, ".gcm"))) continue;
            const std::string full = dir + "/" + name;
            std::FILE* f = std::fopen(full.c_str(), "rb");
            if (!f) continue;
            std::uint8_t head[0x60];
            const bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head);
            std::fclose(f);
            if (!ok || be32(head + 0x1C) != 0xC2339F3Du) continue;
            ImageGame g;
            g.device = ImageDevice::Sd;
            g.path = full;
            g.format = UsbImageFormat::Iso;
            g.id.assign(reinterpret_cast<const char*>(head), 6);
            g.disc_number = head[6];
            g.revision = head[7];
            g.title.assign(reinterpret_cast<const char*>(head + 0x20), strnlen(reinterpret_cast<const char*>(head + 0x20), 0x40));
            g.checked = true;
            g.gamecube = true;
            logf("SD scan: GameCube %s \"%s\" at %s\n", g.id.c_str(), g.title.c_str(), full.c_str());
            out.push_back(std::move(g));
        }
        closedir(inner);
    }
    closedir(games);
    ScanGcPatches(out);
}

bool LaunchGcGame(const GcImage& wanted, const std::vector<PackageChoices>& packages, std::string& error) {
    // Never a partly applied mod: the GameCube loader serves the image as
    // it is and has no way to apply a pack's files yet.
    if (!packages.empty()) {
        error = "packs are not supported for GameCube games yet; switch the game's packs off to play it";
        return false;
    }
    GcImage image = wanted;
    if (!image.patch_source.empty() && !PrepareGcPatch(image, error)) return false;
    logf("GameCube: %s (%s)\n", image.id.c_str(), image.path.c_str());
    return LaunchGcNative(image, error);
}

}  // namespace riftwii::wii
