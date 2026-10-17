// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "riftwii/gccrash.hpp"

#include <cstdio>

#include "gc_rt.h"

namespace riftwii {
namespace {

constexpr std::size_t kRecordBytes = 512;

std::uint32_t word(const std::uint8_t* record, unsigned index) {
    const std::uint8_t* p = record + 4 * index;
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

std::string hex(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(v));
    return text;
}

// gc_on_fatal's exception: the SDK's number from __OSUnhandledException,
// 0x10 + the vector's number for a crash before the game's OS set up its
// handlers (gc_entry.S's gc_dsi_entry), or a jump into RiftWii's own data
// (gc_stray_entry), with CTR in DAR.
std::string exception_name(std::uint32_t exception) {
    if (exception == GC_EXCEPTION_STRAY) return "a jump into RiftWii's own data at SRR0 (DAR holds CTR)";
    if (exception >= 0x10) return "vector " + hex((exception - 0x10) * 0x100) + ", before the game's OS was up";
    switch (exception) {
        case 1: return "machine check";
        case 2: return "DSI (a bad data access)";
        case 3: return "ISI (a jump to a bad address)";
        case 4: return "external interrupt";
        case 5: return "alignment";
        case 6: return "program (an illegal instruction or a trap)";
        case 7: return "floating point unavailable";
        case 8: return "decrementer";
        default: return "exception " + std::to_string(exception);
    }
}

}  // namespace

bool describe_gc_crash(const std::vector<std::uint8_t>& trace, const std::string& game_id, std::string& text) {
    const std::uint8_t* crash = nullptr;
    std::uint32_t newest = 0;
    for (std::size_t at = 0; at + kRecordBytes <= trace.size(); at += kRecordBytes) {
        const std::uint8_t* record = trace.data() + at;
        if (word(record, GC_TR_MAGIC) != GC_TRACE_MAGIC || word(record, GC_TR_WHERE) != GC_TRACE_FATAL) continue;
        const std::uint32_t seq = word(record, GC_TR_SEQ);
        if (crash == nullptr || seq > newest) {
            crash = record;
            newest = seq;
        }
    }
    if (crash == nullptr) return false;
    text = "GameCube game " + (game_id.empty() ? std::string("(unknown)") : game_id) + " crashed: " +
           exception_name(word(crash, GC_TR_EXCEPTION)) + "\n";
    text += "SRR0 " + hex(word(crash, GC_TR_BEAT_PC)) + "  LR " + hex(word(crash, GC_TR_BEAT_LR)) + "  SRR1 " +
            hex(word(crash, GC_TR_BEAT_MSR)) + "  DSISR " + hex(word(crash, GC_TR_DSISR)) + "  DAR " +
            hex(word(crash, GC_TR_DAR)) + "\n";
    text += "r1 " + hex(word(crash, GC_TR_CRASH_R1)) + "  r3 " + hex(word(crash, GC_TR_CRASH_R3)) + "  r4 " +
            hex(word(crash, GC_TR_CRASH_R4)) + "  r5 " + hex(word(crash, GC_TR_CRASH_R5)) + "  r31 " +
            hex(word(crash, GC_TR_CRASH_R31)) + "\n";
    text += "instruction at SRR0 " + hex(word(crash, GC_TR_CRASH_WORD)) + "\ncalled from";
    for (unsigned i = 0; i < 4; ++i) {
        const std::uint32_t lr = word(crash, GC_TR_CRASH_CHAIN + i);
        if (lr == 0) break;
        text += " " + hex(lr);
    }
    text += "\ndisc reads " + std::to_string(word(crash, GC_TR_READS)) + ", SD requests " +
            std::to_string(word(crash, GC_TR_SD_REQUESTS)) + " (" + std::to_string(word(crash, GC_TR_FAILURES)) +
            " failed), pad reads " + std::to_string(word(crash, GC_TR_PAD_READS)) + ", ARAM transfers " +
            std::to_string(word(crash, GC_TR_AR_DMAS)) + ", memory card reads " +
            std::to_string(word(crash, GC_TR_CARD_READS)) + "\n";
    return true;
}

}  // namespace riftwii
