// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// GameCube games in the Channels view: the disc's BNR1/BNR2 banner read
// from a made-up file (its picture and names), a file found on a made-up
// GameCube disc, the banner RiftWii draws in place of an animated one, the
// banner server's addresses, and what the download check refuses.
#include "riftwii/gcbanner.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "riftwii/bnr.hpp"

static int g_failures = 0;
#define EXPECT_TRUE(cond) do { if (!(cond)) { std::cerr << "FAILED: " #cond " at line " << __LINE__ << std::endl; g_failures++; } } while (0)
#define EXPECT_FALSE(cond) do { if (cond) { std::cerr << "FAILED: false expected for " #cond " at line " << __LINE__ << std::endl; g_failures++; } } while (0)
#define EXPECT_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " #a " == " #b " (" << (a) << " != " << (b) << ") at line " << __LINE__ << std::endl; g_failures++; } } while (0)

using namespace riftwii;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
    if (b.size() < at + 2) b.resize(at + 2);
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    put16(b, at, static_cast<std::uint16_t>(v >> 16));
    put16(b, at + 2, static_cast<std::uint16_t>(v));
}
void put_text(Bytes& b, std::size_t at, const std::string& s) {
    if (b.size() < at + s.size()) b.resize(at + s.size());
    std::memcpy(b.data() + at, s.data(), s.size());
}

// Where texel (x, y) of the 96x32 picture lies: 4x4 tiles, left to right
// then down, two bytes a texel.
std::size_t texel_at(int x, int y) {
    const int tile = (y / 4) * (kGcBannerWidth / 4) + x / 4;
    const int inside = (y % 4) * 4 + x % 4;
    return kGcBannerPictureOffset + static_cast<std::size_t>(tile) * 32 + static_cast<std::size_t>(inside) * 2;
}

// A record's fields for language `index`.
void put_record(Bytes& b, int index, const std::string& short_name, const std::string& long_name,
                const std::string& long_maker) {
    const std::size_t at = kGcBannerTextOffset + static_cast<std::size_t>(index) * kGcBannerTextBytes;
    put_text(b, at, short_name);
    put_text(b, at + 0x40, long_name);
    put_text(b, at + 0x80, long_maker);
}

Bytes make_bnr2() {
    Bytes b(kGcBanner2Size, 0);
    put_text(b, 0, "BNR2");
    put16(b, texel_at(0, 0), 0xFFFF);                  // opaque white
    put16(b, texel_at(4, 0), 0x8000 | (31 << 10));      // opaque red, in the second tile
    put16(b, texel_at(1, 5), 0x0F00);                   // alpha mode: transparent, red
    put16(b, texel_at(95, 31), 0x8000 | 31);            // opaque blue, the last texel
    put_record(b, 0, "Sunshine", "Super Mario\nSunshine", "Nintendo");
    put_record(b, 1, "Sunshine", "Super Mario  Sunshine DE", "Nintendo");
    put_record(b, 2, "Court", "", "");  // French: a short name only
    put_record(b, 3, "", "", "");       // Spanish: nothing at all
    put_record(b, 4, "Pok\xE9", "Pok\xE9mon Colosseum", "");
    return b;
}

// A U8 archive holding `files` ({"dir/name", bytes}, one directory each, in order).
Bytes make_u8(const std::vector<std::pair<std::string, Bytes>>& files) {
    struct Node {
        bool dir;
        std::string name;
        std::size_t file;
        std::uint32_t next;
    };
    std::vector<Node> nodes = {{true, "", 0, 0}};
    std::string last_dir;
    std::size_t dir_index = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string& path = files[i].first;
        const std::size_t slash = path.find('/');
        const std::string dir = path.substr(0, slash);
        if (dir != last_dir) {
            if (dir_index) nodes[dir_index].next = static_cast<std::uint32_t>(nodes.size());
            dir_index = nodes.size();
            nodes.push_back({true, dir, 0, 0});
            last_dir = dir;
        }
        nodes.push_back({false, path.substr(slash + 1), i, 0});
    }
    if (dir_index) nodes[dir_index].next = static_cast<std::uint32_t>(nodes.size());
    nodes[0].next = static_cast<std::uint32_t>(nodes.size());
    std::string strings(1, '\0');
    std::vector<std::uint32_t> name_offsets;
    for (const Node& n : nodes) {
        name_offsets.push_back(n.name.empty() ? 0 : static_cast<std::uint32_t>(strings.size()));
        if (!n.name.empty()) strings += n.name + '\0';
    }
    const std::uint32_t header = static_cast<std::uint32_t>(nodes.size() * 12 + strings.size());
    const std::uint32_t data = (0x20 + header + 0x3F) & ~0x3Fu;
    Bytes out(data, 0);
    put32(out, 0, 0x55AA382D);
    put32(out, 4, 0x20);
    put32(out, 8, header);
    put32(out, 12, data);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::size_t at = 0x20 + i * 12;
        put32(out, at, (nodes[i].dir ? 0x01000000u : 0) | name_offsets[i]);
        if (nodes[i].dir) {
            put32(out, at + 4, 0);
            put32(out, at + 8, nodes[i].next);
        } else {
            const Bytes& f = files[nodes[i].file].second;
            put32(out, at + 4, static_cast<std::uint32_t>(out.size()));
            put32(out, at + 8, static_cast<std::uint32_t>(f.size()));
            out.insert(out.end(), f.begin(), f.end());
            while (out.size() % 32) out.push_back(0);
        }
    }
    std::memcpy(out.data() + 0x20 + nodes.size() * 12, strings.data(), strings.size());
    return out;
}

// A Wii-format banner as the server sends one: 0x40 zero bytes, IMET, then
// the U8 archive of meta/*.bin, each an IMD5 header over a (here
// uncompressed) U8 archive.
Bytes make_imet_banner(bool with_banner) {
    const auto bin = [](const std::string& tpl) {
        Bytes b(0x20, 0);
        put_text(b, 0, "IMD5");
        const Bytes inner = make_u8({{"arc/" + tpl, {1, 2, 3, 4}}});
        b.insert(b.end(), inner.begin(), inner.end());
        return b;
    };
    Bytes out(0x600, 0);
    put_text(out, 0x40, "IMET");
    std::vector<std::pair<std::string, Bytes>> meta;
    if (with_banner) meta.push_back({"meta/banner.bin", bin("banner.tpl")});
    meta.push_back({"meta/icon.bin", bin("icon.tpl")});
    const Bytes archive = make_u8(meta);
    out.insert(out.end(), archive.begin(), archive.end());
    return out;
}

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(const Bytes& bytes) : bytes_(bytes) {}
    std::uint64_t size() const override { return bytes_.size(); }
    bool read(std::uint64_t offset, std::uint8_t* destination, std::size_t length) const override {
        if (offset > bytes_.size() || length > bytes_.size() - offset) return false;
        std::memcpy(destination, bytes_.data() + offset, length);
        return true;
    }

private:
    const Bytes& bytes_;
};

}  // namespace

static void test_parse_bnr2() {
    const Bytes file = make_bnr2();
    EXPECT_TRUE(is_gc_banner(file.data(), file.size()));
    GcBanner banner;
    std::string error;
    EXPECT_TRUE(parse_gc_banner(file.data(), file.size(), false, banner, error));
    EXPECT_EQ(banner.rgba.size(), std::size_t(96 * 32 * 4));
    EXPECT_EQ(banner.texts.size(), std::size_t(6));
    const auto pixel = [&](int x, int y, int channel) {
        return static_cast<int>(banner.rgba[(static_cast<std::size_t>(y) * 96 + x) * 4 + channel]);
    };
    EXPECT_EQ(pixel(0, 0, 0), 255);
    EXPECT_EQ(pixel(0, 0, 3), 255);
    EXPECT_EQ(pixel(4, 0, 0), 255);
    EXPECT_EQ(pixel(4, 0, 1), 0);
    EXPECT_EQ(pixel(4, 0, 3), 255);
    EXPECT_EQ(pixel(1, 5, 0), 255);
    EXPECT_EQ(pixel(1, 5, 3), 0);
    EXPECT_EQ(pixel(95, 31, 2), 255);
    EXPECT_EQ(pixel(95, 31, 0), 0);
    // The line break is a space; doubled spaces go.
    EXPECT_EQ(banner.texts[0].long_name, std::string("Super Mario Sunshine"));
    EXPECT_EQ(banner.texts[0].long_maker, std::string("Nintendo"));
    EXPECT_EQ(gc_banner_name(banner, kBnrEnglish), std::string("Super Mario Sunshine"));
    EXPECT_EQ(gc_banner_name(banner, kBnrGerman), std::string("Super Mario Sunshine DE"));
    EXPECT_EQ(gc_banner_name(banner, kBnrFrench), std::string("Court"));             // the short name
    EXPECT_EQ(gc_banner_name(banner, kBnrSpanish), std::string("Super Mario Sunshine"));  // English's
    EXPECT_EQ(gc_banner_name(banner, kBnrItalian), std::string("Pok\xC3\xA9mon Colosseum"));  // Windows-1252
    EXPECT_EQ(gc_banner_name(banner, kBnrJapanese), std::string("Super Mario Sunshine"));
    EXPECT_EQ(gc_banner_name(banner, kBnrKorean), std::string("Super Mario Sunshine"));
}

static void test_parse_bnr1() {
    Bytes file(kGcBanner1Size, 0);
    put_text(file, 0, "BNR1");
    put_record(file, 0, "\x83\x7D\x83\x8A\x83\x49", "", "");  // "マリオ" in Shift-JIS
    GcBanner banner;
    std::string error;
    EXPECT_TRUE(parse_gc_banner(file.data(), file.size(), true, banner, error));
    EXPECT_EQ(banner.texts.size(), std::size_t(1));
    EXPECT_EQ(gc_banner_name(banner, kBnrJapanese), std::string("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA"));
    EXPECT_EQ(gc_banner_name(banner, kBnrGerman), std::string("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA"));
    // The same bytes read as Windows-1252 are not taken as Japanese.
    EXPECT_TRUE(parse_gc_banner(file.data(), file.size(), false, banner, error));
    EXPECT_TRUE(gc_banner_name(banner, kBnrJapanese) != std::string("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA"));
}

static void test_bad_bnr() {
    GcBanner banner;
    std::string error;
    // BNR2 cut to BNR1's length, BNR1 cut short, another magic, nothing.
    Bytes file = make_bnr2();
    file.resize(kGcBanner1Size);
    EXPECT_FALSE(is_gc_banner(file.data(), file.size()));
    EXPECT_FALSE(parse_gc_banner(file.data(), file.size(), false, banner, error));
    EXPECT_EQ(error, std::string("the GameCube banner is too short"));
    file[3] = '1';
    file.resize(kGcBanner1Size - 1);
    EXPECT_FALSE(parse_gc_banner(file.data(), file.size(), false, banner, error));
    file.assign(kGcBanner2Size, 0);
    put_text(file, 0, "BNR3");
    EXPECT_FALSE(parse_gc_banner(file.data(), file.size(), false, banner, error));
    EXPECT_FALSE(parse_gc_banner(nullptr, 0, false, banner, error));
    // A Wii banner is not a GameCube one.
    const Bytes wii = make_imet_banner(true);
    EXPECT_FALSE(is_gc_banner(wii.data(), wii.size()));
}

static void test_disc_file() {
    // A disc: header, an FST at 0x2000 (root, opening.bnr, a folder with a
    // file), the banner at 0x8000.
    const Bytes bnr = make_bnr2();
    Bytes disc(0x8000, 0);
    put_text(disc, 0, "GMSE01");
    put32(disc, 0x1C, 0xC2339F3D);
    const std::string strings = std::string("opening.bnr") + '\0' + "data" + '\0' + "x.bin" + '\0';
    Bytes fst(4 * 12, 0);
    put32(fst, 0, 0x01000000);
    put32(fst, 8, 4);
    put32(fst, 12, 0);  // opening.bnr
    put32(fst, 16, 0x8000);
    put32(fst, 20, static_cast<std::uint32_t>(bnr.size()));
    put32(fst, 24, 0x01000000 | 12);  // data/
    put32(fst, 28, 0);
    put32(fst, 32, 4);
    put32(fst, 36, 17);  // data/x.bin, past the image's end
    put32(fst, 40, 0x10000);
    put32(fst, 44, 16);
    fst.insert(fst.end(), strings.begin(), strings.end());
    std::memcpy(disc.data() + 0x2000, fst.data(), fst.size());
    put32(disc, 0x424, 0x2000);
    put32(disc, 0x428, static_cast<std::uint32_t>(fst.size()));
    disc.insert(disc.end(), bnr.begin(), bnr.end());
    const MemorySource source(disc);
    Bytes out;
    std::string error;
    EXPECT_TRUE(read_gc_disc_file(source, "/opening.bnr", out, error));
    EXPECT_TRUE(out == bnr);
    EXPECT_TRUE(read_gc_disc_file(source, "/OPENING.BNR", out, error));
    EXPECT_FALSE(read_gc_disc_file(source, "/missing.bnr", out, error));
    EXPECT_FALSE(read_gc_disc_file(source, "/data", out, error));          // a folder
    EXPECT_FALSE(read_gc_disc_file(source, "/data/x.bin", out, error));    // past the end
    EXPECT_FALSE(read_gc_disc_file(source, "/opening.bnr", out, error, 0x100));  // over the limit
    // An FST said to run past the image.
    Bytes broken = disc;
    put32(broken, 0x428, 0x100000);
    const MemorySource broken_source(broken);
    EXPECT_FALSE(read_gc_disc_file(broken_source, "/opening.bnr", out, error));
    const Bytes tiny(0x100, 0);
    const MemorySource tiny_source(tiny);
    EXPECT_FALSE(read_gc_disc_file(tiny_source, "/opening.bnr", out, error));
}

static void test_picture_layout() {
    for (const bool icon : {true, false}) {
        const Layout layout = gc_picture_layout(icon);
        EXPECT_EQ(layout.textures.size(), std::size_t(1));
        EXPECT_EQ(layout.textures[0], std::string(kGcPictureTexture));
        const std::vector<LytQuad> quads = layout_quads(layout);
        // The background, the frame's shadow and border, the picture last.
        EXPECT_EQ(quads.size(), std::size_t(4));
        if (quads.size() != 4) continue;
        const LytQuad& background = quads[0];
        const LytQuad& picture = quads[3];
        EXPECT_EQ(layout.panes[static_cast<std::size_t>(picture.pane)].material, 1);
        EXPECT_EQ(layout.materials[1].maps.size(), std::size_t(1));
        const float width = icon ? 112.0f : 384.0f, height = width / 3;
        const float middle = icon ? 0.0f : 90.0f;
        EXPECT_EQ(picture.x[1] - picture.x[0], width);
        EXPECT_EQ(picture.y[0] - picture.y[2], height);
        EXPECT_EQ(picture.x[0], -width / 2);
        EXPECT_EQ(picture.y[0], middle + height / 2);
        // The background covers the whole view.
        EXPECT_TRUE(background.x[0] <= -layout.width / 2 && background.x[1] >= layout.width / 2);
        EXPECT_TRUE(background.y[0] >= layout.height / 2 && background.y[2] <= -layout.height / 2);
        // A 16:9 menu narrows the framed picture back, not the background.
        std::vector<LytQuad> wide;
        LytScratch scratch;
        layout_quads(layout, wide, scratch, 0.75f);
        EXPECT_EQ(wide[3].x[1] - wide[3].x[0], width * 0.75f);
        EXPECT_EQ(wide[0].x[1] - wide[0].x[0], quads[0].x[1] - quads[0].x[0]);
    }
}

static void test_urls() {
    const std::vector<std::string> urls = gc_banner_urls("GMSE01");
    EXPECT_EQ(urls.size(), std::size_t(2));
    if (urls.size() == 2) {
        EXPECT_EQ(urls[0], std::string("http://banner.rc24.xyz/GMSE01.bnr"));
        EXPECT_EQ(urls[1], std::string("http://banner.rc24.xyz/GMS.bnr"));
    }
    EXPECT_EQ(gc_banner_urls("GLMP01").size(), std::size_t(2));
    EXPECT_TRUE(gc_banner_id_ok("G8ME01"));
    // Only six capital letters or digits: nothing else reaches a URL.
    for (const char* bad : {"gmse01", "GMSE0", "GMSE01X", "GMS/01", "../GMS", "GMS?01", "GMS 01", "GMS%2F", ""}) {
        EXPECT_FALSE(gc_banner_id_ok(bad));
        EXPECT_TRUE(gc_banner_urls(bad).empty());
    }
    EXPECT_FALSE(gc_banner_id_ok(std::string("GMS\0E1", 6)));
}

static void test_download_check() {
    std::string error;
    const Bytes good = make_imet_banner(true);
    EXPECT_TRUE(check_gc_banner_download(good.data(), good.size(), error));
    // An icon without the banner a channel opens to.
    const Bytes icon_only = make_imet_banner(false);
    EXPECT_FALSE(check_gc_banner_download(icon_only.data(), icon_only.size(), error));
    // A web page in place of the banner (an error page answered with 200).
    const std::string page = "<!DOCTYPE html><html><body>Not here</body></html>" + std::string(0x800, ' ');
    EXPECT_FALSE(check_gc_banner_download(reinterpret_cast<const std::uint8_t*>(page.data()), page.size(), error));
    // Cut short anywhere: in IMET's header, in the archive's table, in a file.
    for (const std::size_t cut : {std::size_t(0x50), std::size_t(0x610), good.size() - 8}) {
        EXPECT_FALSE(check_gc_banner_download(good.data(), cut, error));
    }
    // A GameCube disc's own banner is not what the server should send.
    const Bytes bnr = make_bnr2();
    EXPECT_FALSE(check_gc_banner_download(bnr.data(), bnr.size(), error));
    // Too big, whatever it holds.
    Bytes big = good;
    big.resize(kGcBannerDownloadMax + 1, 0);
    EXPECT_FALSE(check_gc_banner_download(big.data(), big.size(), error));
    big.resize(kGcBannerDownloadMax);
    EXPECT_TRUE(check_gc_banner_download(big.data(), big.size(), error));
    EXPECT_FALSE(check_gc_banner_download(nullptr, 0, error));
}

int main() {
    test_parse_bnr2();
    test_parse_bnr1();
    test_bad_bnr();
    test_disc_file();
    test_picture_layout();
    test_urls();
    test_download_check();
    if (g_failures == 0) std::cout << "gcbanner: all passed" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
