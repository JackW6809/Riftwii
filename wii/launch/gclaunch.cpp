// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gclaunch.hpp"

#include <dirent.h>
#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "dolboot.h"
#include "log.hpp"
#include "netpacks.hpp"
#include "riftwii/apply.hpp"
#include "riftwii/expand.hpp"
#include "riftwii/fst.hpp"
#include "riftwii/gcoverlay.hpp"
#include "riftwii/patch.hpp"
#include "riftwii/redirect.hpp"
#include "sdfile.hpp"

namespace riftwii::wii {
namespace {

constexpr const char* kNintendont = "sd:/apps/nintendont-riftwii/boot.dol";
constexpr const char* kOverlayPath = "sd:/riftwii/gc/overlay.bin";

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

// The image read as a whole; disc files are ranges of it.
class IsoFile {
public:
    explicit IsoFile(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {}
    ~IsoFile() { close(); }
    bool ok() const { return f_ != nullptr; }
    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }
    std::uint64_t size() {
        if (fseeko(f_, 0, SEEK_END) != 0) return 0;
        return static_cast<std::uint64_t>(ftello(f_));
    }
    bool read(std::uint64_t offset, std::uint8_t* out, std::size_t length) {
        if (fseeko(f_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
        return std::fread(out, 1, length, f_) == length;
    }

private:
    std::FILE* f_;
};

class IsoRange final : public ByteSource {
public:
    IsoRange(IsoFile& iso, std::uint64_t offset, std::uint64_t size) : iso_(iso), offset_(offset), size_(size) {}
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, std::uint8_t* destination, std::size_t length) const override {
        if (offset > size_ || length > size_ - offset) return false;
        return iso_.read(offset_ + offset, destination, length);
    }
    std::uint64_t disc_offset() const { return offset_; }

private:
    IsoFile& iso_;
    std::uint64_t offset_, size_;
};

class SdSource final : public ByteSource {
public:
    explicit SdSource(const Fat32File& file) : file_(file) {}
    std::uint64_t size() const override { return file_.entry.size; }
    bool read(std::uint64_t offset, std::uint8_t* destination, std::size_t length) const override {
        return read_sd_file(file_, offset, destination, length);
    }
    const Fat32File& file() const { return file_; }

private:
    const Fat32File& file_;
};

// Disc files from the image, the packs' files from the card (read raw,
// so their sectors are known for the overlay).
class GcProvider final : public ContentProvider {
public:
    GcProvider(IsoFile& iso, const Fst& fst) : iso_(iso), fst_(fst) {}

    OpenStatus open_disc(const std::string& disc_path, std::unique_ptr<ByteSource>& out, std::string& error) override {
        std::uint32_t index = fst_.find(disc_path, false);
        if (index == Fst::npos) index = fst_.find(disc_path, true);
        if (index == Fst::npos) {
            error = "no such disc file '" + disc_path + "'";
            return OpenStatus::NotFound;
        }
        const FstEntry& e = fst_.entries()[index];
        if (e.is_directory) {
            error = "'" + disc_path + "' is a directory";
            return OpenStatus::Invalid;
        }
        out = std::make_unique<IsoRange>(iso_, e.offset, e.size);
        return OpenStatus::Ok;
    }

    OpenStatus open_external(const std::string& sd_path, std::unique_ptr<ByteSource>& out, std::string& error) override {
        std::string abs = sd_path.compare(0, 3, "sd:") == 0 ? sd_path.substr(3) : sd_path;
        if (abs.empty() || abs[0] != '/') abs = "/" + abs;
        const std::string key = "sd:" + abs;
        auto found = files_.find(key);
        if (found == files_.end()) {
            Fat32File file;
            bool missing = false;
            if (!resolve_sd_file(key, file, missing, error)) {
                const OpenStatus status = missing ? OpenStatus::NotFound : OpenStatus::IoError;
                error = "external file '" + sd_path + "' " + to_string(status) + ": " + error;
                return status;
            }
            if (file.entry.is_directory) {
                error = "'" + key + "' is a directory";
                return OpenStatus::Invalid;
            }
            found = files_.emplace(key, std::move(file)).first;
        }
        out = std::make_unique<SdSource>(found->second);
        return OpenStatus::Ok;
    }

    OpenStatus list_external(const std::string& sd_dir, std::vector<ExternalEntry>& out, std::string& error) override {
        std::string abs = sd_dir.compare(0, 3, "sd:") == 0 ? sd_dir.substr(3) : sd_dir;
        if (abs.empty() || abs[0] != '/') abs = "/" + abs;
        std::vector<Fat32Entry> entries;
        bool missing = false;
        if (!list_sd_directory("sd:" + abs, entries, missing, error))
            return missing ? OpenStatus::NotFound : OpenStatus::IoError;
        out.clear();
        for (const Fat32Entry& e : entries) {
            ExternalEntry x;
            x.name = e.name;
            x.is_directory = e.is_directory;
            out.push_back(std::move(x));
        }
        return OpenStatus::Ok;
    }

private:
    IsoFile& iso_;
    const Fst& fst_;
    std::map<std::string, Fat32File> files_;  // outlives every SdSource
};

// One composed file as overlay pieces: untouched bytes from the image,
// the packs' bytes from their card sectors, zeros.
bool to_pieces(const AppliedFile& file, std::vector<GcPiece>& out, std::string& error) {
    out.clear();
    for (const FlatExtent& x : file.flatten()) {
        if (x.length == 0) continue;
        if (x.kind == FlatExtent::Kind::Zero) {
            GcPiece p;
            p.kind = GcPiece::Kind::Zero;
            p.length = x.length;
            out.push_back(p);
        } else if (x.kind == FlatExtent::Kind::Original) {
            const auto* range = dynamic_cast<const IsoRange*>(x.source);
            if (!range) {
                error = "an untouched piece is not from the image";
                return false;
            }
            GcPiece p;
            p.kind = GcPiece::Kind::Image;
            p.source = range->disc_offset() + x.source_offset;
            p.length = x.length;
            out.push_back(p);
        } else {
            const auto* sd = dynamic_cast<const SdSource*>(x.source);
            if (!sd) {
                error = "a pack's piece is not on the SD card";
                return false;
            }
            std::vector<PlacedRun> runs;
            if (!place_on_fragments(sd->file().fragments, x.source_offset, x.length, runs, error)) return false;
            for (const PlacedRun& r : runs) {
                GcPiece p;
                p.kind = GcPiece::Kind::Device;
                p.source = r.source;
                p.skip = r.skip;
                p.length = r.length;
                out.push_back(p);
            }
        }
    }
    return true;
}

bool read_text(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

// The packs' files as overlay files, in the order the Wii path applies them.
bool plan_packs(const std::vector<PackageChoices>& packages, const std::uint8_t* boot, const Fst& fst,
                GcProvider& provider, std::vector<GcFile>& out, std::string& error) {
    DiscIdentity disc;
    disc.id.assign(reinterpret_cast<const char*>(boot), 6);
    disc.number = boot[6];
    disc.revision = boot[7];
    std::vector<FilePatch> files;
    for (const PackageChoices& pack : packages) {
        const std::string& xml = pack.xml_sd_path;
        std::string text;
        if (!read_text(xml, text)) {
            error = "cannot open " + xml;
            return false;
        }
        Package package;
        if (!parse_package(text, package, error, PackFolderOf(xml))) {
            error = xml + ": " + error;
            return false;
        }
        for (const auto& c : pack.choices) {
            if (!select_choice(package, c.first, c.second, error)) {
                error = xml + ": " + error;
                return false;
            }
        }
        PlanOptions allowed;
        allowed.allow_filename_targets = true;
        allowed.allow_folders = true;
        Plan plan;
        if (!plan_package(package, disc, allowed, plan, error)) {
            error = xml + ": " + error;
            return false;
        }
        // Never a partly applied mod: what GameCube games cannot take yet
        // stops the launch.
        if (!plan.memory.empty() || !plan.savegames.empty() || !plan.shifts.empty()) {
            error = xml + ": memory, savegame and shift patches are not supported for GameCube games yet";
            return false;
        }
        std::vector<std::string> notes;
        std::vector<FilePatch> expanded;
        if (!expand_plan(plan, fst, provider, expanded, notes, error)) {
            error = xml + ": " + error;
            return false;
        }
        for (const std::string& n : notes) logf("GameCube: %s\n", n.c_str());
        logf("GameCube: %s: %u file patch(es)\n", xml.c_str(), static_cast<unsigned>(expanded.size()));
        files.insert(files.end(), expanded.begin(), expanded.end());
    }

    // Patches on the same file apply in order, one on top of the other.
    std::vector<std::string> order;
    std::map<std::string, std::vector<FilePatch>> groups;
    for (const FilePatch& patch : files) {
        std::vector<std::string> targets;
        if (patch.is_filename) {
            std::vector<std::uint32_t> named = fst.find_files_named(patch.disc, true);
            for (std::uint32_t i : named) {
                std::string path;
                if (fst.path_of(i, path)) targets.push_back(path);
            }
        } else {
            targets.push_back(patch.disc);
        }
        for (const std::string& t : targets) {
            FilePatch one = patch;
            one.disc = t;
            one.is_filename = false;
            if (!groups.count(t)) order.push_back(t);
            groups[t].push_back(one);
        }
    }
    out.clear();
    for (const std::string& disc_path : order) {
        std::unique_ptr<AppliedFile> applied;
        if (!apply_patches(groups[disc_path], provider, applied, error)) return false;
        GcFile f;
        f.disc_path = disc_path;
        f.create = fst.find(disc_path, true) == Fst::npos;
        if (!to_pieces(*applied, f.pieces, error)) {
            error = disc_path + ": " + error;
            return false;
        }
        logf("GameCube: %s%s, %llu bytes in %u piece(s)\n", f.create ? "created " : "", disc_path.c_str(),
             static_cast<unsigned long long>(applied->size()), static_cast<unsigned>(f.pieces.size()));
        out.push_back(std::move(f));
    }
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
    return found;
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
}

bool LaunchGcGame(const GcImage& image, const std::vector<PackageChoices>& packages, std::string& error) {
    logf("GameCube: %s (%s)\n", image.id.c_str(), image.path.c_str());
    IsoFile iso(image.path);
    if (!iso.ok()) {
        error = "cannot open " + image.path;
        return false;
    }
    const std::uint64_t iso_size = iso.size();
    std::vector<std::uint8_t> boot(0x440);
    if (!iso.read(0, boot.data(), boot.size())) {
        error = "cannot read " + image.path;
        return false;
    }
    std::vector<std::uint8_t> fst_bytes(be32(&boot[0x428]));
    if (fst_bytes.empty() || !iso.read(be32(&boot[0x424]), fst_bytes.data(), fst_bytes.size())) {
        error = "cannot read the game's FST";
        return false;
    }
    Fst fst;
    if (!Fst::parse(fst_bytes.data(), fst_bytes.size(), false, fst, error)) {
        error = "the game's FST: " + error;
        return false;
    }

    bool overlay = false;
    if (!packages.empty()) {
        GcProvider provider(iso, fst);
        std::vector<GcFile> files;
        if (!plan_packs(packages, boot.data(), fst, provider, files, error)) return false;
        GcOverlayPlan plan;
        std::vector<std::uint8_t> table;
        // The FST travels in the table; one too large for it is not handled yet.
        if (!plan_gc_overlay(boot, fst_bytes, iso_size, files, plan, error) ||
            !finish_gc_overlay(plan, {}, false, table, error))
            return false;
        mkdir("sd:/riftwii/gc", 0777);
        std::FILE* f = std::fopen(kOverlayPath, "wb");
        const bool written = f && std::fwrite(table.data(), 1, table.size(), f) == table.size();
        if (f) std::fclose(f);
        if (!written) {
            error = std::string("cannot write ") + kOverlayPath;
            return false;
        }
        logf("GameCube: %s: %u bytes, %u file(s)\n", kOverlayPath, static_cast<unsigned>(table.size()),
             static_cast<unsigned>(files.size()));
        overlay = true;
    }
    iso.close();

    NinSettings settings;
    settings.game_path = image.path.substr(3);  // without "sd:"
    settings.game_id = image.id;
    settings.overlay = overlay;
    settings.log = true;  // while this is new
    std::vector<std::uint8_t> cfg;
    if (!build_nin_cfg(settings, cfg, error)) return false;

    u32 size = 0;
    u8* dol = dolboot_read(kNintendont, &size, [](u32 n) -> void* { return memalign(32, n); },
                           [](void* p) { std::free(p); });
    if (!dol || !dolboot_valid(dol, size)) {
        error = std::string(kNintendont) + " is missing, damaged or does not fit next to RiftWii";
        std::free(dol);
        return false;
    }
    logf("GameCube: starting %s%s\n", kNintendont, overlay ? " with the overlay" : "");
    LogClose();
    fatUnmount("sd:");
    fatUnmount("usb:");
    AUDIO_StopDMA();
    dolboot_run_args(dol, kNintendont, cfg.data(), static_cast<u32>(cfg.size()));
}

}  // namespace riftwii::wii
