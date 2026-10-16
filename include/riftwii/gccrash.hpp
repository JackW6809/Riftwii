// SPDX-FileCopyrightText: 2026 RiftWii contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace riftwii {

// A GameCube game's crash, from the trace its loader's runtime writes
// (sd:/riftwii/gc/trace.bin, runtime/gc/gc_rt.h's records): the menu turns
// it into sd:/riftwii/gamecrash.txt at its next start, for a problem
// report, as it does a Wii game's crash record.

// The newest crash record in `trace` as text; false when the trace holds
// no crash (or is not a trace).
bool describe_gc_crash(const std::vector<std::uint8_t>& trace, const std::string& game_id, std::string& text);

}  // namespace riftwii
