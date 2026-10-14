// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace riftwii {

// GameCube mods through Nintendont. RiftWii's Nintendont build reads
// /riftwii/gc/overlay.bin (its kernel/RWOverlay.c) and serves the game's
// disc reads through it: the table maps byte ranges of the game's view of
// the disc to sectors of files on the same SD card or USB drive, to other
// places in the image, to zeros, or to a small blob in the table file.
// Nothing about the image changes on the card.
//
// Changed and created files move to a window past the end of the image,
// and so does a rewritten FST; the disc header (0x420-0x440) is served
// from the blob so it points at the new FST.

// One piece of a file's new content; a file is its pieces in order.
// Blob is internal: bytes carried in overlay.bin itself (the header, and
// the FST when it is kept there); a mod file's pieces never use it.
struct GcPiece {
    enum class Kind { Image, Device, Zero, Blob };
    Kind kind = Kind::Zero;
    std::uint64_t length = 0;
    std::uint64_t source = 0;  // Image: byte offset in the image; Device: 512-byte sector; Blob: offset in the blob
    std::uint32_t skip = 0;    // Device: bytes into sector `source` (under 512)
};

struct GcFile {
    std::string disc_path;     // absolute FST path ("/files/x.bin" style, no "files" prefix: "/x.bin")
    bool create = false;       // the path is new; fails if it exists, as an existing path fails without it
    std::vector<GcPiece> pieces;
};

// Built in two steps, because the new FST may live in a file of its own on
// the device, whose sectors are known only once the loader has written it.
struct GcOverlayPlan {
    std::vector<std::uint8_t> fst;  // the new FST: write it to the device (or keep it in the table), then call finish
    // Filled by plan_gc_overlay for finish_gc_overlay.
    std::uint64_t tag = 0;               // disc header bytes 0-7
    std::vector<std::uint8_t> header;    // disc header bytes 0x420-0x440, patched
    std::uint64_t fst_offset = 0;        // where the FST sits in the window
    struct Entry {
        std::uint64_t vstart = 0;
        std::uint64_t length = 0;
        GcPiece piece;
    };
    std::vector<Entry> entries;          // file content in the window, sorted
};

// The disc limits Nintendont's reads have: byte offsets are 32-bit.
constexpr std::uint64_t kGcDiscLimit = 0x100000000ull;
// The most entries and blob bytes RWOverlay.c takes.
constexpr std::uint32_t kGcMaxEntries = 4096;
constexpr std::uint32_t kGcMaxBlob = 0x10000;

// `boot` is the first 0x440 bytes of the image (boot.bin), `fst` its FST
// as stored at boot.bin's 0x424, `image_size` the image file's size.
bool plan_gc_overlay(const std::vector<std::uint8_t>& boot, const std::vector<std::uint8_t>& fst,
                     std::uint64_t image_size, const std::vector<GcFile>& files, GcOverlayPlan& out,
                     std::string& error);

// `fst_pieces` are where the plan's FST was written on the device (Device
// pieces covering exactly plan.fst.size() bytes), or empty to keep the FST
// in overlay.bin itself (it must then fit kGcMaxBlob with the header).
// `usb` says which device Nintendont mounts: every Device piece must be on
// it. Writes the bytes of overlay.bin.
bool finish_gc_overlay(const GcOverlayPlan& plan, const std::vector<GcPiece>& fst_pieces, bool usb,
                       std::vector<std::uint8_t>& table, std::string& error);

// Nintendont's settings (NIN_CFG, version 10: 548 bytes, big endian), as
// a loader passes them in argv[1] to start a game at once.
struct NinSettings {
    std::string game_path;    // on the device Nintendont mounts, without "sd:" ("/games/GLMP01/game.iso")
    std::string game_id;      // the first four characters are used
    bool usb = false;         // the game is on the USB drive
    bool overlay = false;     // read /riftwii/gc/overlay.bin (RiftWii's Nintendont only)
    bool memcard_emu = true;  // memory cards as files in /saves
    bool log = false;         // the kernel's /ndebug.log
};
constexpr std::size_t kNinCfgBytes = 548;
// Fails for a path or ID that does not fit.
bool build_nin_cfg(const NinSettings& settings, std::vector<std::uint8_t>& out, std::string& error);

}  // namespace riftwii
