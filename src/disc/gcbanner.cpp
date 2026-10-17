// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "riftwii/gcbanner.hpp"

#include <cstring>
#include <new>

#include "riftwii/bnr.hpp"
#include "riftwii/fst.hpp"
#include "riftwii/gccard.hpp"
#include "riftwii/tpl.hpp"

namespace riftwii {
namespace {

constexpr const char* kBannerServer = "http://banner.rc24.xyz/";

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// One NUL-padded field of a record as UTF-8. Line breaks (some long names
// are split over two lines for the GameCube's own menu) become spaces, and
// spaces at the ends or doubled are dropped.
std::string field_text(const std::uint8_t* field, std::size_t length, bool shift_jis) {
    std::string raw;
    for (std::size_t i = 0; i < length && field[i] != 0; ++i) {
        const char c = static_cast<char>(field[i]);
        // A Shift-JIS trail byte is 0x40 or more, so these are never half of a character.
        const bool line_break = c == '\n' || c == '\r' || c == '\t';
        raw += line_break ? ' ' : c;
    }
    const std::string text = gc_card_text_utf8(raw, shift_jis);
    std::string tidy;
    for (const char c : text) {
        const bool doubled_space = c == ' ' && (tidy.empty() || tidy.back() == ' ');
        if (!doubled_space) tidy += c;
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

LytPane plain_pane(PaneKind kind, const char* name, int parent, float width, float height) {
    LytPane pane;
    pane.kind = kind;
    pane.name = name;
    pane.parent = parent;
    pane.width = width;
    pane.height = height;
    return pane;
}

// A rectangle shaded from `top` to `bottom`, drawn with material 0 (no texture).
LytPane shaded_pane(const char* name, int parent, float width, float height, LytColor top, LytColor bottom) {
    LytPane pane = plain_pane(PaneKind::Picture, name, parent, width, height);
    pane.vertex[0] = pane.vertex[1] = top;
    pane.vertex[2] = pane.vertex[3] = bottom;
    pane.material = 0;
    return pane;
}

int add_pane(Layout& layout, LytPane pane) {
    const int index = static_cast<int>(layout.panes.size());
    if (pane.parent >= 0) layout.panes[static_cast<std::size_t>(pane.parent)].children.push_back(index);
    layout.panes.push_back(std::move(pane));
    return index;
}

}  // namespace

bool is_gc_banner(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 4) return false;
    if (std::memcmp(data, "BNR1", 4) == 0) return size >= kGcBanner1Size;
    if (std::memcmp(data, "BNR2", 4) == 0) return size >= kGcBanner2Size;
    return false;
}

bool parse_gc_banner(const std::uint8_t* data, std::size_t size, bool shift_jis, GcBanner& out, std::string& error) {
    if (!is_gc_banner(data, size)) {
        error = size >= 4 && (std::memcmp(data, "BNR1", 4) == 0 || std::memcmp(data, "BNR2", 4) == 0)
                    ? "the GameCube banner is too short"
                    : "not a GameCube banner (no BNR1 or BNR2)";
        return false;
    }
    GcBanner banner;
    // The picture through the TPL decoder: a TPL image is the same GX
    // texture, here at a fixed place.
    TplImage picture;
    picture.width = kGcBannerWidth;
    picture.height = kGcBannerHeight;
    picture.format = kTplRGB5A3;
    picture.data_offset = kGcBannerPictureOffset;
    picture.data_size = kGcBannerPictureBytes;
    if (!tpl_to_rgba(data, size, picture, banner.rgba, error)) return false;
    const bool european = data[3] == '2';
    const int languages = european ? 6 : 1;
    for (int i = 0; i < languages; ++i) {
        const std::uint8_t* record = data + kGcBannerTextOffset + static_cast<std::size_t>(i) * kGcBannerTextBytes;
        GcBannerText text;
        text.short_name = field_text(record + 0x00, 0x20, shift_jis);
        text.short_maker = field_text(record + 0x20, 0x20, shift_jis);
        text.long_name = field_text(record + 0x40, 0x40, shift_jis);
        text.long_maker = field_text(record + 0x80, 0x40, shift_jis);
        text.description = field_text(record + 0xC0, 0x80, shift_jis);
        banner.texts.push_back(std::move(text));
    }
    out = std::move(banner);
    return true;
}

std::string gc_banner_name(const GcBanner& banner, int wii_language) {
    // BNR2's records in the Wii's language numbering.
    int record = 0;
    switch (wii_language) {
    case kBnrGerman: record = 1; break;
    case kBnrFrench: record = 2; break;
    case kBnrSpanish: record = 3; break;
    case kBnrItalian: record = 4; break;
    case kBnrDutch: record = 5; break;
    default: record = 0; break;
    }
    if (record >= static_cast<int>(banner.texts.size())) record = 0;
    const auto name_of = [](const GcBannerText& t) { return t.long_name.empty() ? t.short_name : t.long_name; };
    if (banner.texts.empty()) return "";
    const std::string name = name_of(banner.texts[static_cast<std::size_t>(record)]);
    return name.empty() ? name_of(banner.texts[0]) : name;
}

bool read_gc_disc_file(const ByteSource& disc, const std::string& path, std::vector<std::uint8_t>& out,
                       std::string& error, std::size_t max_size) {
    std::uint8_t header[0x440];
    if (disc.size() < sizeof header || !disc.read(0, header, sizeof header)) {
        error = "cannot read the disc's header";
        return false;
    }
    const std::uint32_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
    if (fst_size == 0 || fst_size > (8u << 20) || std::uint64_t(fst_offset) + fst_size > disc.size()) {
        error = "the disc's FST is out of bounds";
        return false;
    }
    std::vector<std::uint8_t> fst_bytes(fst_size);
    if (!disc.read(fst_offset, fst_bytes.data(), fst_bytes.size())) {
        error = "cannot read the disc's FST";
        return false;
    }
    Fst fst;
    if (!Fst::parse(fst_bytes.data(), fst_bytes.size(), false, fst, error)) return false;
    const std::uint32_t index = fst.find(path, true);
    if (index == Fst::npos || fst.entries()[index].is_directory) {
        error = path + " is not on the disc";
        return false;
    }
    const FstEntry& entry = fst.entries()[index];
    if (entry.size > max_size || entry.offset + entry.size > disc.size()) {
        error = path + " is too large or past the image's end";
        return false;
    }
    std::vector<std::uint8_t> bytes(entry.size);
    if (entry.size && !disc.read(entry.offset, bytes.data(), bytes.size())) {
        error = "cannot read " + path;
        return false;
    }
    out.swap(bytes);
    return true;
}

Layout gc_picture_layout(bool icon) {
    Layout layout;
    layout.centered = true;
    layout.width = icon ? 128.0f : 608.0f;
    layout.height = icon ? 96.0f : 456.0f;
    layout.textures.push_back(kGcPictureTexture);
    LytMaterial plain;
    plain.name = "plain";
    LytMaterial picture;
    picture.name = "picture";
    LytTexMap map;
    map.texture = 0;
    picture.maps.push_back(map);
    layout.materials.push_back(plain);
    layout.materials.push_back(picture);

    const int root = add_pane(layout, plain_pane(PaneKind::Null, "RootPane", -1, layout.width, layout.height));
    // A light background, a little past the view's edges, shaded downwards
    // as the Wii Menu's own channels are.
    const LytColor light_top{248, 249, 251, 255}, light_bottom{204, 210, 221, 255};
    add_pane(layout, shaded_pane("background", root, layout.width + 32, layout.height + 32, light_top, light_bottom));

    // The picture in its frame. A GameCube banner is 3:1; it is scaled by
    // a whole number on the banner (4x) and by 7/6 on the tile, where 96
    // pixels would leave most of the tile empty.
    const float scale = icon ? 7.0f / 6.0f : 4.0f;
    const float picture_w = kGcBannerWidth * scale, picture_h = kGcBannerHeight * scale;
    const float border = icon ? 2.0f : 6.0f;
    LytPane group = plain_pane(PaneKind::Null, "frame", root, picture_w, picture_h);
    group.flags = 1 | 4;  // keeps its shape when the menu stretches a banner to 16:9
    // On the banner, above the middle: the menu writes the name under it,
    // clear of the bar along the screen's bottom.
    group.ty = icon ? 0.0f : 90.0f;
    const int frame = add_pane(layout, group);
    const LytColor shadow{0, 0, 0, 64};
    LytPane shade = shaded_pane("shadow", frame, picture_w + 2 * border, picture_h + 2 * border, shadow, shadow);
    shade.tx = border / 2;
    shade.ty = -border;
    add_pane(layout, shade);
    const LytColor dark_top{58, 62, 72, 255}, dark_bottom{30, 33, 40, 255};
    add_pane(layout, shaded_pane("border", frame, picture_w + 2 * border, picture_h + 2 * border, dark_top, dark_bottom));
    LytPane image = plain_pane(PaneKind::Picture, "picture", frame, picture_w, picture_h);
    image.material = 1;
    image.uvs.push_back({0, 0, 1, 0, 0, 1, 1, 1});
    add_pane(layout, image);
    return layout;
}

bool gc_banner_id_ok(const std::string& game_id) {
    if (game_id.size() != 6) return false;
    for (const char c : game_id) {
        const bool letter_or_digit = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!letter_or_digit) return false;
    }
    return true;
}

std::vector<std::string> gc_banner_urls(const std::string& game_id) {
    if (!gc_banner_id_ok(game_id)) return {};
    return {std::string(kBannerServer) + game_id + ".bnr", std::string(kBannerServer) + game_id.substr(0, 3) + ".bnr"};
}

bool check_gc_banner_download(const std::uint8_t* data, std::size_t size, std::string& error) {
    if (size > kGcBannerDownloadMax) {
        error = "the banner is larger than " + std::to_string(kGcBannerDownloadMax) + " bytes";
        return false;
    }
    OpeningBanner banner;
    try {
        if (!parse_opening_bnr(data, size, banner, error, true)) return false;
    } catch (const std::bad_alloc&) {
        error = "out of memory for the banner";
        return false;
    }
    if (banner.banner.empty()) {
        error = "the download has an icon but no banner";
        return false;
    }
    return true;
}

}  // namespace riftwii
