// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <string>
#include <vector>

#include "gc_rt.h"
#include "riftwii/gccrash.hpp"

namespace {

int failures = 0;

#define EXPECT_TRUE(x)                                                   \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::printf("FAILED: %s at line %d\n", #x, __LINE__);        \
            ++failures;                                                  \
        }                                                                \
    } while (0)

void put(std::vector<std::uint8_t>& trace, std::size_t record, unsigned index, std::uint32_t v) {
    std::uint8_t* p = trace.data() + record * 512 + 4 * index;
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void record(std::vector<std::uint8_t>& trace, std::size_t at, std::uint32_t seq, std::uint32_t where) {
    put(trace, at, GC_TR_MAGIC, GC_TRACE_MAGIC);
    put(trace, at, GC_TR_SEQ, seq);
    put(trace, at, GC_TR_WHERE, where);
}

void test_no_crash() {
    std::vector<std::uint8_t> trace(512 * 8, 0);
    record(trace, 0, 1, GC_TRACE_JUMP);
    record(trace, 1, 2, GC_TRACE_READ);
    std::string text;
    EXPECT_TRUE(!riftwii::describe_gc_crash(trace, "GMSE04", text));
    EXPECT_TRUE(!riftwii::describe_gc_crash({}, "GMSE04", text));
    // A record's worth of junk with the crash's number but no magic.
    std::vector<std::uint8_t> junk(512, 0);
    put(junk, 0, GC_TR_WHERE, GC_TRACE_FATAL);
    EXPECT_TRUE(!riftwii::describe_gc_crash(junk, "GMSE04", text));
}

void test_crash() {
    std::vector<std::uint8_t> trace(512 * 8 + 100, 0);  // a short tail is ignored
    record(trace, 0, 1, GC_TRACE_JUMP);
    record(trace, 2, 3, GC_TRACE_FATAL);
    put(trace, 2, GC_TR_EXCEPTION, 6);
    put(trace, 2, GC_TR_BEAT_PC, 0x80001234);
    // A later crash (the ring) is the one described.
    record(trace, 5, 9, GC_TRACE_FATAL);
    put(trace, 5, GC_TR_EXCEPTION, 2);
    put(trace, 5, GC_TR_BEAT_PC, 0x8034ACD8);
    put(trace, 5, GC_TR_DAR, 0x00000004);
    put(trace, 5, GC_TR_CRASH_CHAIN, 0x80005678);
    put(trace, 5, GC_TR_CRASH_CHAIN + 1, 0x80009ABC);
    put(trace, 5, GC_TR_READS, 1801);
    std::string text;
    EXPECT_TRUE(riftwii::describe_gc_crash(trace, "GLMP01", text));
    EXPECT_TRUE(text.find("GameCube game GLMP01 crashed: DSI") == 0);
    EXPECT_TRUE(text.find("SRR0 0x8034acd8") != std::string::npos);
    EXPECT_TRUE(text.find("DAR 0x00000004") != std::string::npos);
    EXPECT_TRUE(text.find("called from 0x80005678 0x80009abc\n") != std::string::npos);
    EXPECT_TRUE(text.find("disc reads 1801") != std::string::npos);
    // Before the game's OS: the vector's address.
    put(trace, 5, GC_TR_EXCEPTION, 0x10 + 3);
    EXPECT_TRUE(riftwii::describe_gc_crash(trace, "", text));
    EXPECT_TRUE(text.find("(unknown) crashed: vector 0x00000300, before the game's OS was up") != std::string::npos);
}

}  // namespace

int main() {
    test_no_crash();
    test_crash();
    if (failures == 0) std::printf("gccrash: all passed\n");
    return failures == 0 ? 0 : 1;
}
