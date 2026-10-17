// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "riftwii/brlyt.hpp"
#include "riftwii/overlay.hpp"

// GameCube games in the Channels view.
//
// A GameCube disc's opening.bnr is the old format, from the public format
// pages (YAGCD's "Banner" and wiibrew's Opening.bnr): "BNR1" (one
// language) or "BNR2" (a European disc: English, German, French, Spanish,
// Italian and Dutch), 0x1C bytes of padding, a 96x32 RGB5A3 picture in
// GX's 4x4 tiles at 0x20, then at 0x1820 one 0x140-byte record per
// language: the short name (0x20 bytes), short maker (0x20), long name
// (0x40), long maker (0x40) and description (0x80), each NUL-padded,
// Shift-JIS on a Japanese disc and Windows-1252 elsewhere.
//
// The community has made Wii-format animated banners (IMET + U8, as a Wii
// disc's) for many GameCube games, and RiiConnect24 hosts them by game ID.
// What the server sends is untrusted: it is stored only when it parses as a
// whole banner (riftwii/bnr.hpp's parse_opening_bnr) within a size bound.
namespace riftwii {

constexpr int kGcBannerWidth = 96, kGcBannerHeight = 32;
constexpr std::size_t kGcBannerPictureOffset = 0x20;
constexpr std::size_t kGcBannerPictureBytes = 96 * 32 * 2;  // RGB5A3, two bytes a texel
constexpr std::size_t kGcBannerTextOffset = 0x1820;
constexpr std::size_t kGcBannerTextBytes = 0x140;
constexpr std::size_t kGcBanner1Size = kGcBannerTextOffset + kGcBannerTextBytes;      // 0x1960
constexpr std::size_t kGcBanner2Size = kGcBannerTextOffset + 6 * kGcBannerTextBytes;  // 0x1FA0

// One language's record, as UTF-8 (a line break in the disc's text becomes
// a space).
struct GcBannerText {
    std::string short_name, short_maker, long_name, long_maker, description;
};

struct GcBanner {
    std::vector<std::uint8_t> rgba;  // 96 x 32 x 4, straight alpha
    std::vector<GcBannerText> texts;  // BNR1: one; BNR2: English, German, French, Spanish, Italian, Dutch
};

// Whether the bytes start with BNR1 or BNR2 and are long enough for it.
bool is_gc_banner(const std::uint8_t* data, std::size_t size);

// `shift_jis`: the disc is Japanese (riftwii/gccard.hpp's gc_card_shift_jis).
bool parse_gc_banner(const std::uint8_t* data, std::size_t size, bool shift_jis, GcBanner& out, std::string& error);

// The game's name for the Wii's language (riftwii/bnr.hpp's BannerLanguage):
// its long name, else its short one, in that language when the banner has
// it and in its first language otherwise. Empty when it gives none.
std::string gc_banner_name(const GcBanner& banner, int wii_language);

// A file from a GameCube disc image (its FST: header offsets at 0x424 and
// 0x428, file offsets in bytes), at most `max_size` bytes.
bool read_gc_disc_file(const ByteSource& disc, const std::string& path, std::vector<std::uint8_t>& out,
                       std::string& error, std::size_t max_size = 64u << 10);

// The banner RiftWii draws for a GameCube game the server has no animated
// banner for: the disc's picture scaled up in a dark frame on a light
// background. `icon`: the Channels tile's 128x96; else the full 608x456
// banner, the picture in its upper part, where the menu writes the game's
// name under it. The picture is texture 0, named kGcPictureTexture; it
// keeps its shape on a 16:9 menu.
constexpr const char* kGcPictureTexture = "gcpicture";
Layout gc_picture_layout(bool icon);

// The banner server's addresses for a game, in the order to ask: its full
// ID, then its first three letters (the server keys most banners by those,
// one for every region). Empty when the ID is not six capital letters or
// digits, so nothing else ever becomes part of a URL.
bool gc_banner_id_ok(const std::string& game_id);
std::vector<std::string> gc_banner_urls(const std::string& game_id);

// The largest banner the download takes (the server's are up to ~1.2 MB).
constexpr std::size_t kGcBannerDownloadMax = 2u << 20;

// Whether a download is a banner the Channels view can play: at most
// kGcBannerDownloadMax bytes, an IMET header, and an icon and a banner that
// unpack. An HTML page, a truncated or broken archive fails, with `error`.
bool check_gc_banner_download(const std::uint8_t* data, std::size_t size, std::string& error);

}  // namespace riftwii
