// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcnative.hpp"

#include <gccore.h>
#include <malloc.h>
#include <sys/stat.h>
#include <ogc/machine/processor.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <fat.h>

#include "d2xsd.hpp"
#include "gc_rt.h"
#include "log.hpp"
#include "riftwii/fat32.hpp"
#include "riftwii/redirect.hpp"
#include "riftwii_gc_bin.h"
#include "sdfile.hpp"
#include "sdio.hpp"

namespace riftwii::wii {

namespace {

constexpr std::uint32_t kMem1Top = 0x81800000;
constexpr std::uint32_t kLoaderStart = 0x80A00000;  // RiftWii's own image (Makefile.wii)
constexpr std::uint32_t kLoaderCeiling = 0x81200000;  // its heap's top (memlimits.hpp)
constexpr std::uint32_t kGameFloor = 0x80003100;
constexpr std::uint32_t kBounceBytes = 0x8000;
constexpr std::uint32_t kBi2Bytes = 0x2000;
constexpr std::uint32_t kStreamBytes = 0x4000;  // disc audio read ahead: about a third of a second
// Broadway in Wii mode: the time base runs at the bus clock / 4 whatever
// the game believes, so the game is told the Wii's clocks and its OSTicks
// conversions come out right.
constexpr std::uint32_t kWiiBusClock = 243000000;
constexpr std::uint32_t kWiiCpuClock = 729000000;

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

struct Section {
    std::uint32_t address = 0;
    std::vector<std::uint8_t> bytes;
};

struct Dol {
    std::vector<Section> text;
    std::vector<Section> data;
    std::uint32_t bss = 0;
    std::uint32_t bss_size = 0;
    std::uint32_t entry = 0;
};

bool read_at(std::FILE* f, std::uint64_t offset, void* out, std::size_t length) {
    if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
    return std::fread(out, 1, length, f) == length;
}

bool read_dol(std::FILE* f, std::uint32_t at, Dol& out, std::string& error) {
    std::uint8_t h[0x100];
    if (!read_at(f, at, h, sizeof(h))) {
        error = "cannot read the game's executable";
        return false;
    }
    for (int i = 0; i < 18; ++i) {
        const std::uint32_t offset = be32(h + 4 * i);
        const std::uint32_t address = be32(h + 0x48 + 4 * i);
        const std::uint32_t size = be32(h + 0x90 + 4 * i);
        if (size == 0) continue;
        if (address < kGameFloor || address + size > kLoaderStart) {
            error = "the game's executable reaches memory RiftWii is still in (not handled yet)";
            return false;
        }
        Section s;
        s.address = address;
        s.bytes.resize(size);
        if (!read_at(f, std::uint64_t(at) + offset, s.bytes.data(), size)) {
            error = "cannot read the game's executable";
            return false;
        }
        (i < 7 ? out.text : out.data).push_back(std::move(s));
    }
    out.bss = be32(h + 0xD8);
    out.bss_size = be32(h + 0xDC);
    out.entry = be32(h + 0xE0);
    if (out.bss_size != 0 && out.bss + out.bss_size > kLoaderStart) {
        error = "the game's memory reaches memory RiftWii is still in (not handled yet)";
        return false;
    }
    return !out.text.empty();
}

// The game's code as words by address, for the search below.
class Code {
public:
    explicit Code(std::vector<Section>& text) : text_(text) {}
    bool has(std::uint32_t a) const { return find(a) != nullptr; }
    std::uint32_t at(std::uint32_t a) const {
        const std::uint8_t* p = find(a);
        return p ? be32(p) : 0;
    }
    void put(std::uint32_t a, std::uint32_t word) {
        std::uint8_t* p = const_cast<std::uint8_t*>(find(a));
        p[0] = word >> 24;
        p[1] = word >> 16;
        p[2] = word >> 8;
        p[3] = word;
    }
    template <class F>
    void each(F f) const {
        for (const Section& s : text_)
            for (std::uint32_t i = 0; i + 4 <= s.bytes.size(); i += 4) f(s.address + i, be32(&s.bytes[i]));
    }

private:
    const std::uint8_t* find(std::uint32_t a) const {
        for (const Section& s : text_)
            if (a >= s.address && a + 4 <= s.address + s.bytes.size()) return &s.bytes[a - s.address];
        return nullptr;
    }
    std::vector<Section>& text_;
};

bool is_lis(std::uint32_t w, std::uint16_t value) { return (w & 0xFC1F0000) == 0x3C000000 && (w & 0xFFFF) == value; }
bool is_oris(std::uint32_t w, std::uint16_t value) { return (w >> 26) == 25 && (w & 0xFFFF) == value; }
std::uint32_t bl_target(std::uint32_t at, std::uint32_t w) {
    if ((w & 0xFC000003) != 0x48000001) return 0;
    std::int32_t d = static_cast<std::int32_t>(w & 0x03FFFFFC);
    if (d & 0x02000000) d -= 0x04000000;
    return at + d;
}

// The DVDLow* functions, found by the DI command each one writes (the
// command words are the drive's, documented on wiibrew and in yagcd).
struct DvdLow {
    std::uint32_t read = 0, seek = 0, disk_id = 0, inquiry = 0, stop_motor = 0, request_error = 0;
    std::uint32_t audio_stream = 0, audio_status = 0, audio_config = 0, reset = 0;
    std::uint32_t set_alarm = 0;
};

// The start of the function holding `at`: the word after the previous blr.
std::uint32_t function_start(const Code& code, std::uint32_t at) {
    for (std::uint32_t a = at - 4; code.has(a) && at - a < 0x400; a -= 4)
        if (code.at(a) == 0x4E800020) return a + 4;
    return 0;
}

bool find_dvd_low(const Code& code, DvdLow& out, std::string& error) {
    std::uint32_t just_read = 0;
    std::map<std::uint32_t, std::vector<std::uint32_t>> calls;  // target -> call sites
    // Each command's candidates; the same words turn up elsewhere in a
    // game, so only those beside the read, seek and disc ID are taken.
    std::map<std::uint32_t*, std::vector<std::uint32_t>> found;
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if (std::uint32_t t = bl_target(a, w)) calls[t].push_back(a);
        auto take = [&](std::uint32_t& slot) {
            const std::uint32_t f = function_start(code, a);
            if (f != 0) found[&slot].push_back(f);
        };
        if (is_lis(w, 0xA800)) {
            // ReadDiskID adds 0x40 to the read command.
            const std::uint32_t next = code.at(a + 4);
            if ((next >> 26) == 14 && (next & 0xFFFF) == 0x40) take(out.disk_id);
            else take(just_read);
        } else if (is_lis(w, 0xAB00)) take(out.seek);
        else if (is_lis(w, 0x1200)) take(out.inquiry);
        else if (is_lis(w, 0xE300)) take(out.stop_motor);
        else if (is_lis(w, 0xE000)) take(out.request_error);
        else if (is_oris(w, 0xE100)) take(out.audio_stream);
        else if (is_oris(w, 0xE200)) take(out.audio_status);
        else if (is_oris(w, 0xE400)) take(out.audio_config);
    });
    // The read, seek and disc ID functions sit together: the read that has
    // a seek within 4 KB.
    for (std::uint32_t r : found[&just_read])
        for (std::uint32_t k : found[&out.seek])
            for (std::uint32_t d : found[&out.disk_id])
                if (just_read == 0 && std::max({r, k, d}) - std::min({r, k, d}) < 0x1000) {
                    just_read = r;
                    out.seek = k;
                    out.disk_id = d;
                }
    if (just_read == 0) {
        error = "the game's disc functions were not found";
        return false;
    }
    // DVDLowRead may be a wrapper round the function that writes the
    // command (older SDKs seek first on some drives): of the two, the one
    // the rest of the game calls.
    const std::uint32_t lo = std::min({just_read, out.seek, out.disk_id}) - 0x1000;
    const std::uint32_t hi = std::max({just_read, out.seek, out.disk_id}) + 0x1000;
    for (std::uint32_t* slot : {&out.inquiry, &out.stop_motor, &out.request_error, &out.audio_stream,
                                &out.audio_status, &out.audio_config})
        for (std::uint32_t f : found[slot])
            if (*slot == 0 && f >= lo && f <= hi) *slot = f;
    auto outside = [&](std::uint32_t target) {
        std::size_t n = 0;
        for (std::uint32_t site : calls[target])
            if (site < lo || site > hi) ++n;
        return n;
    };
    out.read = just_read;
    std::size_t best = outside(just_read);
    for (std::uint32_t site : calls[just_read]) {
        const std::uint32_t f = function_start(code, site);
        if (f != 0 && f >= lo && f <= hi && outside(f) > best) {
            best = outside(f);
            out.read = f;
        }
    }
    // OSSetAlarm: Seek's last call (its timeout alarm).
    for (std::uint32_t a = out.seek; code.has(a) && code.at(a) != 0x4E800020; a += 4)
        if (std::uint32_t t = bl_target(a, code.at(a))) out.set_alarm = t;
    // DVDLowReset: near the others, toggles the DI reset bit in PI's reset
    // register (0xCC003024) with "ori 1" then "ori 5".
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if (a < lo || a > hi || out.reset != 0) return;
        if ((w >> 26) == 24 && (w & 0xFFFF) == 5 && (code.at(a + 4) & 0xFC00FFFF) == 0x90000000) {
            for (std::uint32_t b = a; b > a - 0x80; b -= 4)
                if ((code.at(b) >> 26) == 24 && (code.at(b) & 0xFFFF) == 1) {
                    out.reset = function_start(code, b);
                    break;
                }
        }
    });
    if (out.set_alarm == 0) {
        error = "the game's OSSetAlarm was not found";
        return false;
    }
    return true;
}

// __OSUnhandledException(exception, context, dsisr, dar), the GameCube
// OS's last stop for a crash: the function that prints "Unhandled
// Exception %d". The SDK loads the address of its block of strings once
// (lis/addi) and adds each string's offset, so a function whose base is
// at most 0x200 bytes before the string and that adds the difference.
std::uint32_t find_unhandled_exception(const Code& code, const Dol& dol) {
    static const char kText[] = "Unhandled Exception %d";
    std::uint32_t text = 0;
    for (const Section& s : dol.data) {
        const auto at = std::search(s.bytes.begin(), s.bytes.end(), kText, kText + sizeof(kText));
        if (at != s.bytes.end()) {
            text = s.address + static_cast<std::uint32_t>(at - s.bytes.begin());
            break;
        }
    }
    if (text == 0) return 0;
    std::uint32_t found = 0;
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if (found != 0 || (w & 0xFC1F0000) != 0x3C000000) return;  // lis rX,hi
        const std::uint32_t reg = (w >> 21) & 31;
        for (std::uint32_t b = a + 4; b <= a + 40; b += 4) {
            const std::uint32_t add = code.at(b);
            if ((add >> 26) != 14 || ((add >> 16) & 31) != reg) continue;  // addi rY,rX,lo
            const std::uint32_t base = (w << 16) + static_cast<std::uint32_t>(static_cast<std::int16_t>(add & 0xFFFF));
            if (base > text || text - base >= 0x200) return;
            const std::uint32_t held = (add >> 21) & 31;
            const std::uint32_t wanted = (14u << 26) | (3u << 21) | (held << 16) | (text - base);  // addi r3,rY,off
            if (base == text && held == 3) {
                found = function_start(code, a);
                return;
            }
            for (std::uint32_t c = b + 4; code.has(c) && c < b + 0x300; c += 4)
                if (code.at(c) == wanted) {
                    found = function_start(code, a);
                    return;
                }
            return;
        }
    });
    return found;
}

// The function that stores to each of `addresses` (DSP registers, built
// from "lis rX,0xCC00" and adds, which a walk through each function
// follows):
// - AIInitDMA: the AI DMA start (0xCC005030, 0xCC005032) and length (0xCC005036);
// - ARStartDMA: the ARAM DMA's main memory address (0xCC005020/22), ARAM
//   address (0xCC005024/26) and length with direction (0xCC005028/2A).
std::uint32_t find_dsp_stores(const Code& code, const std::vector<std::uint32_t>& addresses) {
    std::uint32_t found = 0;
    std::uint32_t fn = 0;
    std::uint32_t value[32] = {};
    bool known[32] = {};
    std::uint32_t seen = 0;
    const std::uint32_t all = (1u << addresses.size()) - 1;
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if (found) return;
        if (fn == 0) {
            fn = a;
            std::fill(std::begin(known), std::end(known), false);
            seen = 0;
        }
        const unsigned op = w >> 26, d = (w >> 21) & 31, ra = (w >> 16) & 31;
        const std::int32_t imm = static_cast<std::int16_t>(w & 0xFFFF);
        if (op == 15 && ra == 0) {  // lis
            value[d] = static_cast<std::uint32_t>(imm) << 16;
            known[d] = true;
        } else if (op == 14 && ra != 0 && known[ra]) {  // addi
            value[d] = value[ra] + imm;
            known[d] = true;
        } else if ((op == 44 || op == 45) && known[ra]) {  // sth, sthu
            const std::uint32_t at = value[ra] + imm;
            for (std::size_t i = 0; i < addresses.size(); ++i)
                if (at == addresses[i]) seen |= 1u << i;
            if (seen == all) found = fn;
        } else if (op >= 32 && op <= 43) {  // a load replaces its target
            known[d] = false;
        }
        if (w == 0x4E800020) fn = 0;  // blr: the next word starts a function
    });
    return found;
}

// PADRead(PADStatus status[4]): for each port it cannot read, it stores
// the error into the status's `err` byte (offset 10) and clears the 10
// bytes before it (`stb rE,10(rS)`, then `li r5,10` and a call): the
// function with the most such places, at least three.
std::uint32_t find_pad_read(const Code& code) {
    std::map<std::uint32_t, unsigned> sites;
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if ((w >> 26) != 38 || (w & 0xFFFF) != 10) return;  // stb rX,10(rY)
        for (std::uint32_t b = a + 4; b <= a + 16; b += 4)
            if (code.at(b) == 0x38A0000A) {  // li r5,10
                if (std::uint32_t f = function_start(code, a)) sites[f]++;
                break;
            }
    });
    std::uint32_t best = 0;
    unsigned most = 2;
    for (auto& [f, n] : sites)
        if (n > most) {
            most = n;
            best = f;
        }
    return best;
}

// Sends the game's DI register accesses to `base` (where 0xCC006000 was):
// each "lis rX,0xCC00" whose register is then only used at 0x6000-0x603F
// gets `base`'s upper half. Returns how many.
unsigned redirect_di_registers(Code& code, std::uint32_t base) {
    std::vector<std::uint32_t> patch;
    code.each([&](std::uint32_t a, std::uint32_t w) {
        if (!is_lis(w, 0xCC00)) return;
        const unsigned r = (w >> 21) & 31;
        bool di = false, other = false;
        for (std::uint32_t b = a + 4; b < a + 4 * 40 && code.has(b); b += 4) {
            const std::uint32_t x = code.at(b);
            const unsigned op = x >> 26, d = (x >> 21) & 31, ra = (x >> 16) & 31;
            const std::uint32_t disp = x & 0xFFFF;
            if ((op == 14 || (op >= 32 && op <= 47)) && ra == r) {
                if (disp >= 0x6000 && disp < 0x6040) di = true;
                else other = true;
            }
            if (x == 0x4E800020) break;
            if ((op == 14 || op == 15 || (op >= 32 && op <= 43)) && d == r && !(op >= 32 && op <= 43 && ra == r && false))
                break;  // the register gets another value
        }
        if (di && !other) patch.push_back(a);
    });
    for (std::uint32_t a : patch) code.put(a, (code.at(a) & 0xFFFF0000) | ((base - 0x6000) >> 16));
    return static_cast<unsigned>(patch.size());
}

std::uint32_t branch(std::uint32_t from, std::uint32_t to) { return 0x48000000 | ((to - from) & 0x03FFFFFC); }

void store32(std::uint32_t a, std::uint32_t v) { *reinterpret_cast<volatile std::uint32_t*>(a) = v; }

}  // namespace

bool LaunchGcNative(const GcImage& image, std::string& error) {
    logf("GameCube (native): %s (%s)\n", image.id.c_str(), image.path.c_str());
    // Read while the card is still the file system's (before it is opened raw).
    struct stat st_press;
    const bool test_presses = stat("sd:/riftwii/gc/press", &st_press) == 0;
    Fat32File file;
    if (!resolve_sd_file(image.path, file, error)) return false;
    std::vector<PlacedRun> placed;
    if (!place_on_fragments(file.fragments, 0, file.entry.size, placed, error)) return false;

    std::FILE* f = std::fopen(image.path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + image.path;
        return false;
    }
    std::vector<std::uint8_t> boot(0x440), bi2(kBi2Bytes);
    Dol dol;
    bool ok = read_at(f, 0, boot.data(), boot.size()) && read_at(f, 0x440, bi2.data(), bi2.size());
    if (!ok) error = "cannot read " + image.path;
    ok = ok && read_dol(f, be32(&boot[0x420]), dol, error);
    const std::uint32_t fst_size = be32(&boot[0x428]);
    const std::uint32_t fst_max = std::max(be32(&boot[0x42C]), fst_size);
    std::vector<std::uint8_t> fst(fst_size);
    if (ok && (fst.empty() || !read_at(f, be32(&boot[0x424]), fst.data(), fst.size()))) {
        error = "cannot read the game's FST";
        ok = false;
    }
    std::fclose(f);
    if (!ok) return false;

    Code code(dol.text);
    DvdLow low;
    if (!find_dvd_low(code, low, error)) return false;
    logf("GameCube: DVDLowRead 0x%08x, Seek 0x%08x, ReadDiskID 0x%08x, Inquiry 0x%08x, StopMotor 0x%08x, "
         "RequestError 0x%08x, AudioStream 0x%08x, AudioStatus 0x%08x, AudioConfig 0x%08x, Reset 0x%08x, "
         "OSSetAlarm 0x%08x\n",
         low.read, low.seek, low.disk_id, low.inquiry, low.stop_motor, low.request_error, low.audio_stream,
         low.audio_status, low.audio_config, low.reset, low.set_alarm);

    // Top of MEM1, as the apploader leaves it: the FST, then (ours) BI2's
    // copy, the blob, its runs and its bounce buffer; the arena below.
    const gc_rt_header& h = *reinterpret_cast<const gc_rt_header*>(riftwii_gc_bin);
    if (riftwii_gc_bin_size < sizeof(h) || h.magic != GC_RT_MAGIC || h.version != GC_RT_VERSION ||
        h.size > riftwii_gc_bin_size) {
        error = "the GameCube blob is damaged";
        return false;
    }
    const std::uint32_t runs_bytes = static_cast<std::uint32_t>(placed.size() * sizeof(gc_rt_run));
    const std::uint32_t fst_addr = (kMem1Top - fst_max) & ~31u;
    const std::uint32_t bi2_addr = fst_addr - kBi2Bytes;
    const std::uint32_t bounce_addr = (bi2_addr - kBounceBytes) & ~31u;
    const std::uint32_t adp_addr = (bounce_addr - kStreamBytes) & ~31u;
    const std::uint32_t stream_addr = (adp_addr - sizeof(gc_rt_stream)) & ~31u;
    const std::uint32_t trace_addr = (stream_addr - 512) & ~31u;
    const std::uint32_t runs_addr = (trace_addr - runs_bytes) & ~31u;
    const std::uint32_t blob_addr = (runs_addr - h.size) & ~31u;
    // The DI registers' stand-in: its address must end in 0x6000, as the
    // game adds that to the upper half it loads.
    const std::uint32_t di_regs = ((blob_addr - 0x40 - 0x6000) & ~0xFFFFu) + 0x6000;
    const std::uint32_t arena_hi = di_regs;
    if (arena_hi < kLoaderCeiling) {
        error = "the game's FST is too large";
        return false;
    }

    // The blob and its table, straight into place (above RiftWii's heap).
    std::memcpy(reinterpret_cast<void*>(blob_addr), riftwii_gc_bin, h.size);
    auto* ctx = reinterpret_cast<gc_rt_context*>(blob_addr + h.context_offset);
    auto* runs = reinterpret_cast<gc_rt_run*>(runs_addr);
    std::uint32_t disc = 0;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        runs[i].disc = disc;
        runs[i].length = static_cast<std::uint32_t>(placed[i].length);
        runs[i].source = static_cast<std::uint32_t>(placed[i].source);
        runs[i].skip = placed[i].skip;
        runs[i].kind = GC_RUN_SD;
        disc += runs[i].length;
    }
    std::memcpy(reinterpret_cast<void*>(fst_addr), fst.data(), fst.size());
    std::memcpy(reinterpret_cast<void*>(bi2_addr), bi2.data(), bi2.size());

    // The trace (gc_rt.h): one cluster's worth of zeroed records, its
    // sectors given to the blob, read back after a run that went wrong.
    std::uint32_t trace_sector = 0, trace_count = 0;
    {
        static const char kTrace[] = "sd:/riftwii/gc/trace.bin";
        std::vector<std::uint8_t> zeros(16 << 10);
        std::FILE* t = std::fopen(kTrace, "wb");
        const bool written = t && std::fwrite(zeros.data(), 1, zeros.size(), t) == zeros.size();
        if (t) std::fclose(t);
        forget_sd_layout();  // the raw lookups' folder listings predate it
        Fat32File tf;
        std::string why;
        if (written && resolve_sd_file(kTrace, tf, why) && !tf.fragments.empty()) {
            trace_sector = static_cast<std::uint32_t>(tf.fragments[0].sector);
            trace_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(32, tf.fragments[0].sector_count));
        }
        logf("GameCube: trace %s (sector %u, %u records)%s%s\n", trace_count ? "on" : "off", trace_sector, trace_count,
             why.empty() ? "" : ": ", why.c_str());
    }

    // The card, opened and selected; the blob reads it after the jump.
    // IOS lets one owner have the slot: the file system lets go first (as
    // a Wii game's launch does), and the log goes on on screen only.
    logf("Releasing the SD card (the log ends here; the rest is on screen)\n");
    LogClose();
    fatUnmount("sd:");
    sd_interface()->shutdown();
    sdio::Card card;
    if (!sdio::open_card(card, error)) return false;
    if (card.d2x) {
        error = "the SD card is in use through d2x (not handled yet)";
        return false;
    }
    ctx->sdio_fd = static_cast<std::uint32_t>(card.fd);
    ctx->sdhc = card.sdhc ? 1 : 0;
    ctx->runs = runs_addr;
    ctx->run_count = static_cast<std::uint32_t>(placed.size());
    ctx->set_alarm = low.set_alarm;
    ctx->alarm_entry = blob_addr + h.alarm_entry;
    ctx->bounce = bounce_addr;
    ctx->bounce_bytes = kBounceBytes;
    ctx->delay_ticks = 2000;  // about 30 microseconds
    ctx->beat_ticks = 30375000;  // half a second of the bus clock / 4
    ctx->di_regs = di_regs;
    std::memset(reinterpret_cast<void*>(di_regs), 0, 0x40);
    auto* stream = reinterpret_cast<gc_rt_stream*>(stream_addr);
    std::memset(stream, 0, sizeof(*stream));
    stream->buf = adp_addr;
    stream->buf_bytes = kStreamBytes;
    stream->sample = 28;
    ctx->stream = stream_addr;
    ctx->trace_sector = trace_sector;
    ctx->trace_count = trace_count;
    ctx->trace_buf = trace_addr;
    ctx->card_rca = card.rca;

    // The hooks, in the staged code.
    auto hook = [&](std::uint32_t fn, std::uint32_t entry) {
        if (fn != 0) code.put(fn, branch(fn, blob_addr + entry));
    };
    // A hook that runs the original afterwards: its first word to the
    // replay slot, then the jump to its second.
    auto replay_hook = [&](std::uint32_t fn, std::uint32_t entry, std::uint32_t replay, std::uint32_t back) {
        store32(blob_addr + replay, code.at(fn));
        store32(blob_addr + back, 0x3D800000 | ((fn + 4) >> 16));
        store32(blob_addr + back + 4, 0x618C0000 | ((fn + 4) & 0xFFFF));
        hook(fn, entry);
    };
    hook(low.read, h.hook_read);
    hook(low.seek, h.hook_seek);
    hook(low.disk_id, h.hook_disk_id);
    hook(low.inquiry, h.hook_inquiry);
    hook(low.stop_motor, h.hook_cb_r3);
    hook(low.request_error, h.hook_cb_r3);
    hook(low.audio_status, h.hook_audio_status);
    hook(low.audio_config, h.hook_cb_r5);
    hook(low.audio_stream, h.hook_audio_stream);
    const unsigned di_patches = redirect_di_registers(code, di_regs);
    const std::uint32_t ai_dma = find_dsp_stores(code, {0xCC005030, 0xCC005032, 0xCC005036});
    if (ai_dma != 0) {
        // Its first word to the replay slot, then the jump to its second.
        const std::uint32_t replay = blob_addr + h.replay_ai_dma;
        const std::uint32_t back = blob_addr + h.continue_ai_dma;
        const std::uint32_t first = code.at(ai_dma);
        if ((first >> 26) == 18 || (first >> 26) == 16) {
            error = "AIInitDMA starts with a branch (not handled yet)";
            return false;
        }
        store32(replay, first);
        store32(back, 0x3D800000 | ((ai_dma + 4) >> 16));        // lis r12, hi
        store32(back + 4, 0x618C0000 | ((ai_dma + 4) & 0xFFFF));  // ori r12, r12, lo
        hook(ai_dma, h.hook_ai_dma);
    }
    // Test presses (Dolphin runs with no controller to press): only when
    // sd:/riftwii/gc/press exists.
    // PADRead, for the trace's heartbeat and (sd:/riftwii/gc/press: Dolphin
    // runs with no controller to press) the test presses.
    const std::uint32_t pad_read = find_pad_read(code);
    if (pad_read != 0) {
        replay_hook(pad_read, h.hook_pad_read, h.replay_pad_read, h.continue_pad_read);
        if (test_presses) ctx->flags |= GC_FLAG_PRESS;
    }
    const std::uint32_t ar_dma = find_dsp_stores(code, {0xCC005020, 0xCC005022, 0xCC005024, 0xCC005026,
                                                         0xCC005028, 0xCC00502A});
    if (ar_dma != 0) replay_hook(ar_dma, h.hook_ar_dma, h.replay_ar_dma, h.continue_ar_dma);
    // A crash shows itself on the screen and in the trace instead of a
    // blank halt.
    const std::uint32_t unhandled = find_unhandled_exception(code, dol);
    hook(unhandled, h.hook_fatal);
    logf("GameCube: __OSUnhandledException 0x%08x\n", unhandled);
    logf("GameCube: PADRead 0x%08x%s, ARStartDMA 0x%08x\n", pad_read, test_presses ? " (test presses)" : "", ar_dma);
    logf("GameCube: %u DI register access(es) sent to 0x%08x; AIInitDMA 0x%08x%s\n", di_patches, di_regs, ai_dma,
         ai_dma ? "" : " (not found: no disc audio streaming)");
    if (low.reset != 0) code.put(low.reset, 0x4E800020);  // blr: the drive is not there to reset

    const std::uint32_t tv_mode = image.id.size() > 3 && (image.id[3] == 'P' || image.id[3] == 'D' ||
                                                          image.id[3] == 'F' || image.id[3] == 'S' ||
                                                          image.id[3] == 'I' || image.id[3] == 'X')
                                      ? 1  // PAL
                                      : 0;
    logf("GameCube: entry 0x%08x, FST 0x%08x, blob 0x%08x (%u bytes), %u run(s), arena top 0x%08x, card fd %d %s\n",
         dol.entry, fst_addr, blob_addr, static_cast<unsigned>(h.size), static_cast<unsigned>(placed.size()),
         arena_hi, card.fd, card.sdhc ? "SDHC" : "SDSC");
    DCFlushRange(reinterpret_cast<void*>(di_regs), kMem1Top - di_regs);
    ICInvalidateRange(reinterpret_cast<void*>(blob_addr), h.size);

    LogClose();
    // The frame stays on screen, painted at each step below (gc_rt.h's
    // colours) until the game sets up its own video.
    const std::uint32_t xfb = reinterpret_cast<std::uint32_t>(VIDEO_GetCurrentFramebuffer());
    const std::uint32_t xfb_bytes = 640 * 480 * 2;
    ctx->xfb = xfb;
    ctx->xfb_bytes = xfb_bytes;
    DCFlushRange(ctx, sizeof(*ctx));
    auto paint = [&](std::uint32_t colour) {
        volatile std::uint32_t* p = reinterpret_cast<volatile std::uint32_t*>((xfb & 0x3FFFFFFF) | 0xC0000000);
        for (std::uint32_t i = 0; i < xfb_bytes / 4; ++i) p[i] = colour;
    };
    VIDEO_Flush();
    VIDEO_WaitVSync();
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    u32 level;
    _CPU_ISR_Disable(level);
    (void)level;
    paint(GC_COLOUR_BLUE);

    // Low memory as a GameCube's start-up leaves it for the game.
    std::memset(reinterpret_cast<void*>(0x80000000), 0, kGameFloor - 0x80000000);
    for (const Section& s : dol.text) std::memcpy(reinterpret_cast<void*>(s.address), s.bytes.data(), s.bytes.size());
    for (const Section& s : dol.data) std::memcpy(reinterpret_cast<void*>(s.address), s.bytes.data(), s.bytes.size());
    if (dol.bss_size != 0) {
        // Sections may sit inside the BSS range; zero only around them.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
        for (const Section& s : dol.data) used.emplace_back(s.address, s.address + s.bytes.size());
        for (std::uint32_t a = dol.bss; a < dol.bss + dol.bss_size; ++a) {
            bool inside = false;
            for (auto& u : used)
                if (a >= u.first && a < u.second) inside = true;
            if (!inside) *reinterpret_cast<volatile std::uint8_t*>(a) = 0;
        }
    }
    paint(GC_COLOUR_GREEN);
    std::memcpy(reinterpret_cast<void*>(0x80000000), boot.data(), 0x20);
    store32(0x80000020, 0x0D15EA5E);
    store32(0x80000024, 1);
    store32(0x80000028, 0x01800000);
    store32(0x8000002C, 0x00000003);  // a retail console
    store32(0x80000034, arena_hi);
    store32(0x80000038, fst_addr);
    store32(0x8000003C, fst_max);
    store32(0x800000CC, tv_mode);
    store32(0x800000D0, 0x01000000);  // ARAM
    store32(0x800000EC, kMem1Top);
    store32(0x800000F0, 0x01800000);
    store32(0x800000F4, bi2_addr);
    store32(0x800000F8, kWiiBusClock);
    store32(0x800000FC, kWiiCpuClock);
    // IOS's replies stay polled: no IPC interrupt reaches the game.
    store32(0xCD000004, 0);
    // Nor any other of Hollywood's interrupts: the game's OS knows none of
    // them and could not clear one (HW_PPCIRQMASK, then the flags acked).
    store32(0xCD000034, 0);
    store32(0xCD000030, 0xFFFFFFFF);
    // A first trace record through the blob's own IPC, as the game will
    // use it: written here, the card and the polling work on this console.
    paint(GC_COLOUR_YELLOW);
    reinterpret_cast<void (*)(std::uint32_t)>(blob_addr + h.trace_now)(GC_TRACE_JUMP);
    DCFlushRange(reinterpret_cast<void*>(0x80000000), kLoaderStart - 0x80000000);
    ICInvalidateRange(reinterpret_cast<void*>(0x80000000), kLoaderStart - 0x80000000);
    // Trial: a write watch (DABR, translated stores) on 0x800031a0, where
    // a Wii run finds memset's code overwritten; the store that does it
    // raises a DSI, which the crash hook shows.
    {
        const std::uint32_t watch = 0x800031A0 | 0x4 | 0x2;  // BT | DW
        asm volatile("mtspr 1013, %0" : : "r"(watch) : "memory");
    }
    reinterpret_cast<void (*)()>(dol.entry)();
    return false;
}

}  // namespace riftwii::wii
