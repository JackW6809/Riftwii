// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcpatch.hpp"

#include <dirent.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include "log.hpp"
#include "riftwii/vcdiff.hpp"

namespace riftwii::wii {

namespace {

// The Wii's share of memory for preparing: an 8 MB window, its copy
// window read at once up to 8 MB, larger ones through 8 MB of cache.
VcdiffLimits wii_limits() {
    VcdiffLimits l;
    l.max_window = 32u << 20;
    l.whole_copy_window = 8u << 20;
    l.cache_block = 64u << 10;
    l.cache_blocks = 128;
    return l;
}

struct File {
    std::FILE* f = nullptr;
    std::uint64_t size = 0;
    explicit File(const std::string& path, const char* mode = "rb") : f(std::fopen(path.c_str(), mode)) {
        if (f && mode[0] == 'r' && fseeko(f, 0, SEEK_END) == 0) {
            size = static_cast<std::uint64_t>(ftello(f));
            fseeko(f, 0, SEEK_SET);
        }
    }
    ~File() {
        if (f) std::fclose(f);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    VcdiffReadFn reader() {
        return [this](std::uint64_t at, void* out, std::size_t n) {
            return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n;
        };
    }
};

bool ends_with(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}

std::vector<std::string> list_patches() {
    std::vector<std::string> out;
    DIR* d = opendir(kGcPatchDir);
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name[0] == '.') continue;
        if (ends_with(name, ".xdelta") || ends_with(name, ".vcdiff")) out.push_back(std::string(kGcPatchDir) + "/" + name);
    }
    closedir(d);
    return out;
}

}  // namespace

void ScanGcPatches(std::vector<ImageGame>& games) {
    const std::vector<std::string> patches = list_patches();
    if (patches.empty()) return;
    std::vector<ImageGame> found;
    for (const std::string& path : patches) {
        File patch(path);
        if (!patch.f) continue;
        bool listed = false;
        for (const ImageGame& g : games) {
            if (!g.gamecube || !g.gc_patch_source.empty() || g.device != ImageDevice::Sd) continue;
            File original(g.path);
            if (!original.f) continue;
            std::vector<std::uint8_t> head;
            bool checked = false;
            std::string error;
            if (!vcdiff_head(patch.reader(), patch.size, original.reader(), original.size, 0x60, head, checked, error,
                             wii_limits()))
                continue;  // not this one's original
            if (!checked) {
                logf("GameCube patch %s: no checksum to tell its original by; not listed\n", path.c_str());
                break;
            }
            ImageGame x;
            x.device = ImageDevice::Sd;
            x.path = path;
            x.format = UsbImageFormat::Iso;
            x.id.assign(reinterpret_cast<const char*>(head.data()), 6);
            x.disc_number = head[6];
            x.revision = head[7];
            x.title.assign(reinterpret_cast<const char*>(head.data() + 0x20),
                           strnlen(reinterpret_cast<const char*>(head.data() + 0x20), 0x40));
            x.checked = true;
            x.gamecube = true;
            x.gc_patch_source = g.path;
            bool have = false;
            for (const ImageGame& other : games)
                if (other.gamecube && other.id == x.id && other.gc_patch_source.empty()) have = true;
            logf("GameCube patch %s: makes %s \"%s\" from %s%s\n", path.c_str(), x.id.c_str(), x.title.c_str(),
                 g.path.c_str(), have ? " (already there: not listed)" : "");
            if (!have) found.push_back(std::move(x));
            listed = true;
            break;
        }
        if (!listed) logf("GameCube patch %s: none of the GameCube games is its original\n", path.c_str());
    }
    for (ImageGame& x : found) games.push_back(std::move(x));
}

bool PrepareGcPatch(GcImage& image, std::string& error) {
    const std::string dir = "sd:/games/" + image.id;
    const std::string final_path = dir + "/game.iso";
    const std::string part = dir + "/game.iso.part";
    File patch(image.path), original(image.patch_source);
    if (!patch.f || !original.f) {
        error = "cannot open " + std::string(patch.f ? image.patch_source : image.path);
        return false;
    }
    struct statvfs vfs;
    if (statvfs("sd:/", &vfs) == 0) {
        const std::uint64_t free_bytes = std::uint64_t(vfs.f_bavail) * vfs.f_frsize;
        if (free_bytes < original.size + (64u << 20)) {
            error = "the SD card needs " + std::to_string((original.size >> 20) + 64) + " MB free to prepare this game (" +
                    std::to_string(free_bytes >> 20) + " MB free)";
            return false;
        }
    }
    mkdir("sd:/games", 0777);
    mkdir(dir.c_str(), 0777);
    std::remove(part.c_str());
    logf("GameCube: preparing %s from %s and %s, once; it is kept as %s\n", image.id.c_str(), image.patch_source.c_str(),
         image.path.c_str(), final_path.c_str());
    bool ok;
    VcdiffStats stats;
    {
        File out(part, "w+b");
        if (!out.f) {
            error = "cannot write " + part;
            return false;
        }
        std::setvbuf(out.f, nullptr, _IOFBF, 256 << 10);
        std::uint64_t written = 0;
        int shown = -1;
        const u64 start = gettime();
        ok = apply_vcdiff(
            patch.reader(), patch.size, original.reader(), original.size,
            [&](const std::uint8_t* d, std::size_t n) {
                if (std::fwrite(d, 1, n, out.f) != n) return false;
                written += n;
                return true;
            },
            [&](std::uint64_t at, void* dst, std::size_t n) {
                // Windows that copy from earlier output read it back.
                std::fflush(out.f);
                const bool r = fseeko(out.f, static_cast<off_t>(at), SEEK_SET) == 0 && std::fread(dst, 1, n, out.f) == n;
                fseeko(out.f, 0, SEEK_END);
                return r;
            },
            [&](std::uint64_t done, std::uint64_t total) {
                const int pct = static_cast<int>(done * 100 / (total ? total : 1));
                if (pct / 5 != shown / 5) {
                    shown = pct;
                    logf("Preparing %s: %d%% (%llu MB, %u s)\n", image.id.c_str(), pct,
                         static_cast<unsigned long long>(written >> 20),
                         static_cast<unsigned>(ticks_to_secs(gettime() - start)));
                }
                return true;
            },
            error, wii_limits(), &stats);
        if (ok && std::fflush(out.f) != 0) {
            ok = false;
            error = "cannot write " + part;
        }
    }
    if (!ok) {
        std::remove(part.c_str());
        error = "preparing " + image.id + ": " + error;
        return false;
    }
    if (std::rename(part.c_str(), final_path.c_str()) != 0) {
        std::remove(part.c_str());
        error = "cannot rename " + part;
        return false;
    }
    logf("GameCube: %s prepared (the original read %llu times, %llu MB)\n", final_path.c_str(),
         static_cast<unsigned long long>(stats.source_reads), static_cast<unsigned long long>(stats.source_bytes >> 20));
    image.path = final_path;
    image.patch_source.clear();
    return true;
}

}  // namespace riftwii::wii
