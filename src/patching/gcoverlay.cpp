// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "riftwii/gcoverlay.hpp"

#include <algorithm>

#include "riftwii/fst.hpp"
#include "rtable.h"

namespace riftwii {

namespace {

constexpr std::uint64_t kAlign = 0x8000;  // window slots start on a 32 KiB boundary, like the disc's own files
constexpr std::uint32_t kBlobMagic = 0x52574742u;  // 'RWGB'

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(std::uint8_t(v >> 24));
    out.push_back(std::uint8_t(v >> 16));
    out.push_back(std::uint8_t(v >> 8));
    out.push_back(std::uint8_t(v));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    put32(out, std::uint32_t(v >> 32));
    put32(out, std::uint32_t(v));
}
void poke32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}
std::uint64_t round_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }

std::uint64_t pieces_size(const std::vector<GcPiece>& pieces) {
    std::uint64_t total = 0;
    for (const GcPiece& p : pieces) total += p.length;
    return total;
}

// Lays `pieces` out from `at`, then zeros up to a 32-byte multiple so a
// read the DVD driver rounds up never falls through to the image.
bool place(std::vector<GcOverlayPlan::Entry>& entries, std::uint64_t at, const std::vector<GcPiece>& pieces,
           std::string& error) {
    std::uint64_t cursor = at;
    for (const GcPiece& p : pieces) {
        if (p.length == 0) continue;
        if (p.kind == GcPiece::Kind::Device && p.skip >= RT_SECTOR_BYTES) {
            error = "a device piece starts past its sector";
            return false;
        }
        GcOverlayPlan::Entry e;
        e.vstart = cursor;
        e.length = p.length;
        e.piece = p;
        entries.push_back(e);
        cursor += p.length;
    }
    const std::uint64_t padded = round_up(cursor - at, 32) + at;
    if (padded > cursor) {
        GcOverlayPlan::Entry e;
        e.vstart = cursor;
        e.length = padded - cursor;
        entries.push_back(e);  // a Zero piece
    }
    return true;
}

}  // namespace

bool plan_gc_overlay(const std::vector<std::uint8_t>& boot, const std::vector<std::uint8_t>& fst_image,
                     std::uint64_t image_size, const std::vector<GcFile>& files, GcOverlayPlan& out,
                     std::string& error) {
    if (boot.size() < 0x440) {
        error = "the disc header is short";
        return false;
    }
    if (be32(boot.data() + 0x1C) != 0xC2339F3Du) {
        error = "not a GameCube disc";
        return false;
    }
    Fst fst;
    if (!Fst::parse(fst_image.data(), fst_image.size(), false, fst, error)) {
        error = "the disc's FST: " + error;
        return false;
    }

    // The window starts past everything the image holds.
    std::uint64_t end = image_size;
    const std::uint64_t fst_end = std::uint64_t(be32(boot.data() + 0x424)) + be32(boot.data() + 0x428);
    end = std::max(end, fst_end);
    for (const FstEntry& e : fst.entries())
        if (!e.is_directory) end = std::max(end, e.offset + e.size);
    std::uint64_t cursor = round_up(end, kAlign);

    GcOverlayPlan plan;
    plan.tag = (std::uint64_t(be32(boot.data())) << 32) | be32(boot.data() + 4);

    // Give every file its slot; created files first go into the table so
    // the indices of existing ones are looked up afterwards.
    std::vector<std::uint64_t> slots(files.size());
    std::vector<FstNewFile> created;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const GcFile& f = files[i];
        const std::uint64_t size = pieces_size(f.pieces);
        if (size > 0xFFFFFFFFull) {
            error = "'" + f.disc_path + "' is too large for the FST";
            return false;
        }
        const std::uint32_t index = fst.find(f.disc_path, true);
        if (f.create && index != Fst::npos) {
            error = "'" + f.disc_path + "' already exists";
            return false;
        }
        for (const GcPiece& p : f.pieces)
            if (p.kind == GcPiece::Kind::Blob) {
                error = "'" + f.disc_path + "' has a piece kept in the table";
                return false;
            }
        if (!f.create && (index == Fst::npos || fst.entries()[index].is_directory)) {
            error = "no such disc file '" + f.disc_path + "'";
            return false;
        }
        slots[i] = cursor;
        if (!place(plan.entries, cursor, f.pieces, error)) return false;
        cursor = round_up(cursor + size, kAlign);
        if (cursor > kGcDiscLimit) {
            error = "the mod's files do not fit below 4 GiB";
            return false;
        }
        if (f.create) created.push_back(FstNewFile{f.disc_path, slots[i], std::uint32_t(size)});
    }
    if (!created.empty()) {
        std::vector<std::uint32_t> indices;
        if (!fst.create_files(created, indices, error)) return false;
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].create) continue;
        const std::uint32_t index = fst.find(files[i].disc_path, true);
        if (!fst.set_file_extent(index, slots[i], std::uint32_t(pieces_size(files[i].pieces)), error)) return false;
    }

    if (!fst.serialize(plan.fst, error)) return false;
    plan.fst.resize(round_up(plan.fst.size(), 32), 0);
    plan.fst_offset = cursor;
    if (plan.fst_offset + plan.fst.size() > kGcDiscLimit) {
        error = "the new FST does not fit below 4 GiB";
        return false;
    }

    // The header points at the new FST. Its maximum size is what the
    // apploader reserves (the largest of a game's discs), so it only grows.
    plan.header.assign(boot.begin() + 0x420, boot.begin() + 0x440);
    poke32(plan.header.data() + 4, std::uint32_t(plan.fst_offset));
    poke32(plan.header.data() + 8, std::uint32_t(plan.fst.size()));
    if (be32(plan.header.data() + 12) < plan.fst.size()) poke32(plan.header.data() + 12, std::uint32_t(plan.fst.size()));

    out = std::move(plan);
    error.clear();
    return true;
}

bool finish_gc_overlay(const GcOverlayPlan& plan, const std::vector<GcPiece>& fst_pieces, bool usb,
                       std::vector<std::uint8_t>& table, std::string& error) {
    // The blob: the patched header, then the FST when it travels in the table.
    std::vector<std::uint8_t> blob = plan.header;
    std::vector<GcPiece> fst_at = fst_pieces;
    if (fst_pieces.empty()) {
        if (blob.size() + plan.fst.size() > kGcMaxBlob) {
            error = "the FST is too large to keep in the table; write it to the device";
            return false;
        }
        GcPiece inline_fst;
        inline_fst.kind = GcPiece::Kind::Blob;
        inline_fst.source = blob.size();
        inline_fst.length = plan.fst.size();
        fst_at.push_back(inline_fst);
        blob.insert(blob.end(), plan.fst.begin(), plan.fst.end());
    } else {
        for (const GcPiece& p : fst_pieces)
            if (p.kind != GcPiece::Kind::Device) {
                error = "the FST must be on the device";
                return false;
            }
    }
    if (pieces_size(fst_at) != plan.fst.size()) {
        error = "the FST's pieces do not cover it";
        return false;
    }
    std::vector<GcOverlayPlan::Entry> entries;
    GcOverlayPlan::Entry header;
    header.vstart = 0x420;
    header.length = plan.header.size();
    header.piece.kind = GcPiece::Kind::Blob;  // the blob starts with the header
    header.piece.length = plan.header.size();
    entries.push_back(header);
    entries.insert(entries.end(), plan.entries.begin(), plan.entries.end());
    if (!place(entries, plan.fst_offset, fst_at, error)) return false;
    if (entries.size() > kGcMaxEntries) {
        error = "the mod changes too many pieces (" + std::to_string(entries.size()) + "; the most is " +
                std::to_string(kGcMaxEntries) + ")";
        return false;
    }

    std::vector<std::uint8_t> body;
    std::uint64_t last_end = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const GcOverlayPlan::Entry& e = entries[i];
        if (e.vstart < last_end || e.length == 0 || e.length > 0xFFFFFFFFull ||
            e.vstart + e.length > kGcDiscLimit) {
            error = "overlay entries overlap or run past 4 GiB";
            return false;
        }
        last_end = e.vstart + e.length;
        std::uint32_t kind = RT_KIND_ZERO;
        std::uint64_t source = 0;
        std::uint32_t skip = 0;
        if (e.piece.kind == GcPiece::Kind::Blob) {
            kind = RT_KIND_MEM;
            source = e.piece.source;
        } else if (e.piece.kind == GcPiece::Kind::Image) {
            kind = RT_KIND_DISC;
            source = e.piece.source;
        } else if (e.piece.kind == GcPiece::Kind::Device) {
            kind = usb ? RT_KIND_USB : RT_KIND_SD;
            source = e.piece.source;
            skip = e.piece.skip;
        }
        put64(body, e.vstart);
        put64(body, source);
        put32(body, std::uint32_t(e.length));
        body.push_back(std::uint8_t(skip >> 8));
        body.push_back(std::uint8_t(skip));
        body.push_back(std::uint8_t(kind));
        body.push_back(0);
    }

    table.clear();
    put32(table, RT_MAGIC);
    put32(table, RT_VERSION);
    put32(table, std::uint32_t(entries.size()));
    put32(table, 0xFFFFFFFFu);
    put64(table, plan.tag);
    put32(table, rt_crc32(body.data(), body.size()));
    put32(table, 0);
    table.insert(table.end(), body.begin(), body.end());
    put32(table, kBlobMagic);
    put32(table, std::uint32_t(blob.size()));
    table.insert(table.end(), blob.begin(), blob.end());
    error.clear();
    return true;
}

bool build_nin_cfg(const NinSettings& s, std::vector<std::uint8_t>& out, std::string& error) {
    // Nintendont's common/include/CommonConfig.h: magic, version, config
    // bits, video mode, language, the game path (255 bytes), the cheat path
    // (255), then max pads, game ID, memory card size and three more bytes,
    // and the Wii U gamepad slot.
    constexpr std::uint32_t kAutoBoot = 1u << 7, kMemcardEmu = 1u << 3, kLog = 1u << 12, kUsb = 1u << 10;
    constexpr std::uint32_t kRiftWiiOverlay = 1u << 30;
    if (s.game_path.empty() || s.game_path[0] != '/' || s.game_path.size() > 254) {
        error = "the game's path does not fit Nintendont's settings";
        return false;
    }
    if (s.game_id.size() < 4) {
        error = "the game ID is too short";
        return false;
    }
    out.assign(kNinCfgBytes, 0);
    std::uint32_t config = kAutoBoot;
    if (s.memcard_emu) config |= kMemcardEmu;
    if (s.log) config |= kLog;
    if (s.usb) config |= kUsb;
    if (s.overlay) config |= kRiftWiiOverlay;
    poke32(&out[0], 0x01070CF6u);
    poke32(&out[4], 10);
    poke32(&out[8], config);
    poke32(&out[12], 0);            // video: automatic
    poke32(&out[16], 0xFFFFFFFFu);  // language: the console's
    std::copy(s.game_path.begin(), s.game_path.end(), out.begin() + 20);
    poke32(&out[532], 4);  // pads
    poke32(&out[536], be32(reinterpret_cast<const std::uint8_t*>(s.game_id.data())));
    out[540] = 2;  // memory card: 251 blocks
    error.clear();
    return true;
}

}  // namespace riftwii
