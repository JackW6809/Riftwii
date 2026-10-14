// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// GameCube overlay: a made-up image and card, the table built for them,
// and a reader that serves disc reads from the table the way Nintendont's
// RWOverlay.c does. The game's view (header, FST, every file) is the oracle.
#include "riftwii/fst.hpp"
#include "riftwii/gcoverlay.hpp"
#include "rtable.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;
#define EXPECT_TRUE(cond) do { if (!(cond)) { std::cerr << "FAILED: " #cond " at line " << __LINE__ << std::endl; g_failures++; } } while (0)
#define EXPECT_FALSE(cond) do { if (cond) { std::cerr << "FAILED: false expected for " #cond " at line " << __LINE__ << std::endl; g_failures++; } } while (0)
#define EXPECT_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " #a " == " #b " (" << (a) << " != " << (b) << ") at line " << __LINE__ << std::endl; g_failures++; } } while (0)

using namespace riftwii;
using Bytes = std::vector<std::uint8_t>;

static std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
static std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }
static void poke32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = std::uint8_t(v >> 24);
    b[at + 1] = std::uint8_t(v >> 16);
    b[at + 2] = std::uint8_t(v >> 8);
    b[at + 3] = std::uint8_t(v);
}
static Bytes pattern(std::size_t n, std::uint8_t seed) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = std::uint8_t(seed + i * 7 + (i >> 8));
    return b;
}

// A tiny disc: "/a.bin" at 0x10000 (0x100 bytes), "/dir/b.bin" at 0x20000
// (0x80 bytes), "/keep.bin" at 0x30000 (0x40 bytes). The FST sits at 0x8000.
struct Disc {
    Bytes image;
    Bytes fst;
};

static Disc make_disc() {
    Disc d;
    d.image.assign(0x40000, 0xEE);
    std::memcpy(d.image.data(), "GTSTE8", 6);
    d.image[6] = 0;  // disc 1
    d.image[7] = 2;  // version
    poke32(d.image, 0x1C, 0xC2339F3Du);
    // FST: root, a.bin, dir, b.bin, keep.bin
    const char* names[] = {"a.bin", "dir", "b.bin", "keep.bin"};
    Bytes strings;
    std::vector<std::uint32_t> name_offsets;
    for (const char* n : names) {
        name_offsets.push_back(std::uint32_t(strings.size()));
        strings.insert(strings.end(), n, n + std::strlen(n) + 1);
    }
    Bytes fst(5 * 12, 0);
    auto entry = [&](int i, bool dir, std::uint32_t name, std::uint32_t a, std::uint32_t b) {
        poke32(fst, i * 12, (dir ? 0x01000000u : 0) | name);
        poke32(fst, i * 12 + 4, a);
        poke32(fst, i * 12 + 8, b);
    };
    entry(0, true, 0, 0, 5);
    entry(1, false, name_offsets[0], 0x10000, 0x100);
    entry(2, true, name_offsets[1], 0, 4);
    entry(3, false, name_offsets[2], 0x20000, 0x80);
    entry(4, false, name_offsets[3], 0x30000, 0x40);
    fst.insert(fst.end(), strings.begin(), strings.end());
    d.fst = fst;
    std::memcpy(d.image.data() + 0x8000, fst.data(), fst.size());
    poke32(d.image, 0x420, 0x2440 + 0x2000);  // main.dol offset (unused here)
    poke32(d.image, 0x424, 0x8000);
    poke32(d.image, 0x428, std::uint32_t(fst.size()));
    poke32(d.image, 0x42C, std::uint32_t(fst.size()));
    Bytes a = pattern(0x100, 1), b = pattern(0x80, 2), k = pattern(0x40, 3);
    std::memcpy(d.image.data() + 0x10000, a.data(), a.size());
    std::memcpy(d.image.data() + 0x20000, b.data(), b.size());
    std::memcpy(d.image.data() + 0x30000, k.data(), k.size());
    return d;
}

// Serves reads like RWOverlay.c: entries over the image, the blob, the card.
struct Reader {
    const Bytes* image = nullptr;
    const Bytes* card = nullptr;
    bool usb = false;
    struct E { std::uint64_t vstart, source; std::uint32_t length, skip, kind; };
    std::vector<E> entries;
    Bytes blob;
    std::uint64_t tag = 0;

    bool load(const Bytes& t) {
        if (t.size() < 32 || be32(t.data()) != RT_MAGIC || be32(t.data() + 4) != RT_VERSION) return false;
        const std::uint32_t n = be32(t.data() + 8);
        tag = be64(t.data() + 16);
        if (t.size() < 32 + n * 24 + 8) return false;
        if (rt_crc32(t.data() + 32, n * 24) != be32(t.data() + 24)) return false;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint8_t* p = t.data() + 32 + i * 24;
            E e{be64(p), be64(p + 8), be32(p + 16), std::uint32_t((p[20] << 8) | p[21]), p[22]};
            if (p[23] != 0) return false;
            entries.push_back(e);
        }
        const std::uint8_t* b = t.data() + 32 + n * 24;
        if (be32(b) != 0x52574742u) return false;
        const std::uint32_t size = be32(b + 4);
        if (t.size() != 32 + n * 24 + 8 + size) return false;
        blob.assign(b + 8, b + 8 + size);
        return true;
    }
    std::uint8_t byte(std::uint64_t at) const {
        for (const E& e : entries) {
            if (at < e.vstart || at >= e.vstart + e.length) continue;
            const std::uint64_t into = at - e.vstart;
            switch (e.kind) {
            case RT_KIND_ZERO: return 0;
            case RT_KIND_MEM: return blob.at(e.source + into);
            case RT_KIND_DISC: return image->at(e.source + into);
            case RT_KIND_SD:
            case RT_KIND_USB:
                if ((e.kind == RT_KIND_USB) != usb) throw 1;
                return card->at(e.source * 512 + e.skip + into);
            default: throw 2;
            }
        }
        return at < image->size() ? (*image)[at] : 0xAB;  // past the image: junk the test must never see
    }
    Bytes read(std::uint64_t at, std::uint64_t n) const {
        Bytes out(n);
        for (std::uint64_t i = 0; i < n; ++i) out[i] = byte(at + i);
        return out;
    }
};

// Writes `data` to the card at a sector + skip and returns it as a piece.
static GcPiece on_card(Bytes& card, std::uint64_t sector, std::uint32_t skip, const Bytes& data) {
    const std::uint64_t at = sector * 512 + skip;
    if (card.size() < at + data.size()) card.resize(at + data.size(), 0);
    std::memcpy(card.data() + at, data.data(), data.size());
    GcPiece p;
    p.kind = GcPiece::Kind::Device;
    p.source = sector;
    p.skip = skip;
    p.length = data.size();
    return p;
}

static Bytes file_through(const Reader& r, const Fst& fst, const std::string& path, bool& found) {
    const std::uint32_t i = fst.find(path, false);
    found = i != Fst::npos;
    if (!found) return {};
    return r.read(fst.entries()[i].offset, fst.entries()[i].size);
}

static void test_full_mod(bool usb, bool fst_in_table) {
    Disc d = make_disc();
    Bytes card;
    const Bytes new_a = pattern(0x1234, 9);   // grows past its slot
    const Bytes new_c = pattern(0x77, 5);     // a created file, odd length
    const Bytes tail = pattern(0x20, 4);

    std::vector<GcFile> files(3);
    files[0].disc_path = "/A.BIN";  // the SDK's paths are case-insensitive
    files[0].pieces.push_back(on_card(card, 10, 100, new_a));
    files[1].disc_path = "/dir/new/c.txt";
    files[1].create = true;
    files[1].pieces.push_back(on_card(card, 40, 0, new_c));
    // b.bin: its first 0x40 original bytes, then new bytes from the card.
    files[2].disc_path = "/dir/b.bin";
    GcPiece orig;
    orig.kind = GcPiece::Kind::Image;
    orig.source = 0x20000;
    orig.length = 0x40;
    files[2].pieces.push_back(orig);
    files[2].pieces.push_back(on_card(card, 50, 3, tail));

    GcOverlayPlan plan;
    std::string error;
    Bytes boot(d.image.begin(), d.image.begin() + 0x440);
    EXPECT_TRUE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));
    if (!error.empty()) std::cerr << error << std::endl;
    EXPECT_EQ(plan.fst.size() % 32, 0u);

    // The loader writes the FST to the card (here split over two runs), or
    // keeps it in the table.
    std::vector<GcPiece> fst_pieces;
    if (!fst_in_table) {
        const std::size_t half = 0x30;
        Bytes first(plan.fst.begin(), plan.fst.begin() + half), second(plan.fst.begin() + half, plan.fst.end());
        fst_pieces = {on_card(card, 60, 7, first), on_card(card, 80, 0, second)};
    }
    Bytes table;
    EXPECT_TRUE(finish_gc_overlay(plan, fst_pieces, usb, table, error));
    if (!error.empty()) std::cerr << error << std::endl;

    Reader r;
    r.image = &d.image;
    r.card = &card;
    r.usb = usb;
    EXPECT_TRUE(r.load(table));
    EXPECT_EQ(r.tag, be64(d.image.data()));

    // The game's boot: header first, then the FST the header names.
    const Bytes header = r.read(0, 0x440);
    EXPECT_TRUE(std::memcmp(header.data(), d.image.data(), 0x420) == 0);
    EXPECT_EQ(be32(header.data() + 0x420), be32(d.image.data() + 0x420));
    const std::uint32_t fst_at = be32(header.data() + 0x424), fst_size = be32(header.data() + 0x428);
    EXPECT_TRUE(fst_at >= d.image.size());
    EXPECT_EQ(fst_size, plan.fst.size());
    EXPECT_TRUE(be32(header.data() + 0x42C) >= fst_size);
    const Bytes seen_fst = r.read(fst_at, fst_size);
    Fst fst;
    EXPECT_TRUE(Fst::parse(seen_fst.data(), seen_fst.size(), false, fst, error));

    bool found = false;
    EXPECT_TRUE(file_through(r, fst, "/a.bin", found) == new_a);
    EXPECT_TRUE(found);
    EXPECT_TRUE(file_through(r, fst, "/dir/new/c.txt", found) == new_c);
    EXPECT_TRUE(found);
    Bytes want_b(d.image.begin() + 0x20000, d.image.begin() + 0x20040);
    want_b.insert(want_b.end(), tail.begin(), tail.end());
    EXPECT_TRUE(file_through(r, fst, "/dir/b.bin", found) == want_b);
    Bytes keep(d.image.begin() + 0x30000, d.image.begin() + 0x30040);
    EXPECT_TRUE(file_through(r, fst, "/keep.bin", found) == keep);
    EXPECT_TRUE(found);

    // Every moved file's tail up to 32 bytes reads as zero, never as junk.
    const std::uint32_t c = fst.find("/dir/new/c.txt", false);
    const Bytes rounded = r.read(fst.entries()[c].offset, (fst.entries()[c].size + 31) & ~31u);
    for (std::size_t i = new_c.size(); i < rounded.size(); ++i) EXPECT_EQ(int(rounded[i]), 0);
}

static void test_refusals() {
    Disc d = make_disc();
    Bytes boot(d.image.begin(), d.image.begin() + 0x440);
    GcOverlayPlan plan;
    std::string error;

    std::vector<GcFile> files(1);
    files[0].disc_path = "/missing.bin";
    EXPECT_FALSE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));
    files[0].disc_path = "/a.bin";
    files[0].create = true;
    EXPECT_FALSE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));
    files[0].disc_path = "/dir";
    files[0].create = false;
    EXPECT_FALSE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));

    Bytes wii = boot;
    poke32(wii, 0x1C, 0);
    files.clear();
    EXPECT_FALSE(plan_gc_overlay(wii, d.fst, d.image.size(), files, plan, error));

    // The FST must be on the device Nintendont mounts.
    EXPECT_TRUE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));
    GcPiece img;
    img.kind = GcPiece::Kind::Image;
    img.length = plan.fst.size();
    Bytes table;
    EXPECT_FALSE(finish_gc_overlay(plan, {img}, false, table, error));
    GcPiece short_piece;
    short_piece.kind = GcPiece::Kind::Device;
    short_piece.length = plan.fst.size() - 4;
    EXPECT_FALSE(finish_gc_overlay(plan, {short_piece}, false, table, error));

    // Bytes kept in the table are the header's and the FST's alone.
    files.resize(1);
    files[0].disc_path = "/a.bin";
    files[0].create = false;
    GcPiece blob;
    blob.kind = GcPiece::Kind::Blob;
    blob.length = 4;
    files[0].pieces = {blob};
    EXPECT_FALSE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));

    // An FST too large for the table must go to the device.
    files.clear();
    EXPECT_TRUE(plan_gc_overlay(boot, d.fst, d.image.size(), files, plan, error));
    plan.fst.resize(kGcMaxBlob, 0);
    EXPECT_FALSE(finish_gc_overlay(plan, {}, false, table, error));
}

static void test_no_changes() {
    // No files: only the FST and header move; every file reads as before.
    Disc d = make_disc();
    Bytes boot(d.image.begin(), d.image.begin() + 0x440);
    GcOverlayPlan plan;
    std::string error;
    EXPECT_TRUE(plan_gc_overlay(boot, d.fst, d.image.size(), {}, plan, error));
    Bytes card;
    Bytes table;
    EXPECT_TRUE(finish_gc_overlay(plan, {on_card(card, 2, 0, plan.fst)}, false, table, error));
    Reader r;
    r.image = &d.image;
    r.card = &card;
    EXPECT_TRUE(r.load(table));
    const Bytes header = r.read(0x420, 0x20);
    const Bytes seen_fst = r.read(be32(header.data() + 4), be32(header.data() + 8));
    Fst fst;
    EXPECT_TRUE(Fst::parse(seen_fst.data(), seen_fst.size(), false, fst, error));
    bool found = false;
    EXPECT_TRUE(file_through(r, fst, "/a.bin", found) == Bytes(d.image.begin() + 0x10000, d.image.begin() + 0x10100));
}

static void test_nin_cfg() {
    NinSettings s;
    s.game_path = "/games/GLMP01/game.iso";
    s.game_id = "GLMP01";
    s.overlay = true;
    Bytes cfg;
    std::string error;
    EXPECT_TRUE(build_nin_cfg(s, cfg, error));
    EXPECT_EQ(cfg.size(), kNinCfgBytes);
    EXPECT_EQ(be32(cfg.data()), 0x01070CF6u);
    EXPECT_EQ(be32(cfg.data() + 4), 10u);
    EXPECT_EQ(be32(cfg.data() + 8), (1u << 7) | (1u << 3) | (1u << 30));
    EXPECT_TRUE(std::memcmp(cfg.data() + 20, "/games/GLMP01/game.iso", 23) == 0);  // NUL included
    EXPECT_EQ(be32(cfg.data() + 536), 0x474C4D50u);
    EXPECT_EQ(int(cfg[540]), 2);
    s.game_path = "games/x.iso";
    EXPECT_FALSE(build_nin_cfg(s, cfg, error));
    s.game_path = "/" + std::string(254, 'a');
    EXPECT_FALSE(build_nin_cfg(s, cfg, error));
}

int main() {
    test_nin_cfg();
    test_full_mod(false, false);
    test_full_mod(true, false);
    test_full_mod(false, true);
    test_refusals();
    test_no_changes();
    if (g_failures) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "gcoverlay: all tests passed" << std::endl;
    return 0;
}
