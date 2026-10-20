# SPDX-FileCopyrightText: 2026 RiftWii contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prints the records of a GameCube trace (sd:/riftwii/gc/trace.bin).

The blob writes a 512-byte record now and then while a GameCube game runs
(runtime/gc/gc_rt.h, "The trace"). The word names come from the
gc_trace_word enum in gc_rt.h, so this reader follows the header.

    py -3.13 tools/gc_trace.py trace.bin [--all]

Without --all, words that are zero in every record are left out. In a run
with the test remote (sd:/riftwii/gc/test/fakeremote, or fakepro for a
Wii U Pro Controller) the projection_callers words hold its results instead
(gc_bt.h's fake_log).
"""
import argparse
import os
import re
import struct

HEADER = os.path.join(os.path.dirname(__file__), '..', 'runtime', 'gc', 'gc_rt.h')
MAGIC = 0x52575452  # "RWTR"
RECORD_BYTES = 512
WHERE = {1: 'read', 2: 'pad', 3: 'ai', 4: 'ar', 5: 'jump', 6: 'beat', 7: 'fatal', 8: 'command', 9: 'leave', 10: 'restart',
         11: 'padpath'}
WHERE_PADPATH = 11  # its records use the gc_padpath_word enum (the pad path's counters, issue #135)
# Counters read better in decimal; everything else (addresses, registers) in hex.
# gc_video.h's GC_VI_WIDTH_KEPT, set in the vi_mode word.
VI_WIDTH_KEPT = 0x10000
DECIMAL = {'seq', 'commands', 'reads', 'read_bytes', 'sd_requests', 'failures', 'pad_reads', 'stream_mixes',
           'stream_refills', 'ai_dmas', 'ar_dmas', 'alarms', 'beats', 'guard_hits', 'bt_events', 'bt_acl',
           'card_reads', 'card_flushes', 'card_dirty', 'vi_configures', 'projections', 'stream_starved',
           'stream_fetch_busy', 'stream_buffered', 'orthographics', 'ar_waits', 'ar_wait_longest',
           'ar_wait_timeouts', 'restarts'}


def word_names(enum_name='gc_trace_word', end_name='GC_TR_WORDS', prefix='GC_TR_'):
    """The enum's names in order, with the gaps its '= X + n' entries leave."""
    text = open(HEADER).read()
    body = re.search(r'enum %s \{(.*?)\};' % enum_name, text, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    names = []
    for entry in (e.strip() for e in body.split(',')):
        if not entry:
            continue
        name, _, value = entry.partition('=')
        name = name.strip()
        if value:
            # An array of words: its first name stands for all of them,
            # numbered here (crash_chain, crash_chain_1, ...).
            base, _, extra = value.partition('+')
            first = names.index(base.strip())
            index = first + int(extra or 0, 0)
            while len(names) < index:
                names.append('%s_%d' % (base.strip(), len(names) - first))
        names.append(name)
    names.remove(end_name)
    return [n.replace(prefix, '', 1).lower() for n in names]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('trace')
    parser.add_argument('--all', action='store_true', help='show words that are zero everywhere too')
    args = parser.parse_args()

    # Two layouts share the file: the main record, and the pad path's (where=padpath).
    layouts = {False: word_names(), True: word_names('gc_padpath_word', 'GC_PT_WORDS', 'GC_PT_')}
    data = open(args.trace, 'rb').read()
    records = []
    for at in range(0, len(data) - RECORD_BYTES + 1, RECORD_BYTES):
        words = struct.unpack_from('>%dI' % (RECORD_BYTES // 4), data, at)
        if words[0] == MAGIC:
            records.append(words)
    records.sort(key=lambda words: words[1])
    print('%d record(s)' % len(records))

    shown = {}
    for padpath in (False, True):
        of_kind = [r for r in records if (r[2] == WHERE_PADPATH) == padpath]
        shown[padpath] = [i for i, name in enumerate(layouts[padpath])
                          if name not in ('magic', 'tbu') and (args.all or any(r[i] for r in of_kind))]
    for record in records:
        fields = []
        padpath = record[2] == WHERE_PADPATH
        names = layouts[padpath]
        for i in shown[padpath]:
            name, value = names[i], record[i]
            if name == 'where':
                fields.append('where=%s' % WHERE.get(value, value))
            elif name in DECIMAL:
                fields.append('%s=%d' % (name, value))
            elif name == 'vi_mode' and value & VI_WIDTH_KEPT:
                # The picture width setting did nothing in this mode.
                fields.append('vi_mode=0x%x (width kept: the frame buffer is wider)' % value)
            else:
                fields.append('%s=0x%x' % (name, value))
        print(' '.join(fields))


if __name__ == '__main__':
    main()
