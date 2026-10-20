/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A diagnostic for a GameCube game's pad path (issue #135: on a Wii U a
 * sideways Wii Remote, or a remote with a Classic Controller, connects and
 * reports but does not play): fixed-size counters and last values, kept in
 * struct gc_bt (MEM2, sized by sizeof at the launch) and copied into the
 * trace record (gc_rt.h's GC_TR_PD_*) by gc_trace, so a problem report says
 * where a press was lost between the parsed Wii Remote report and what the
 * game's PADRead left in its PADStatus. Only counts and samples: nothing
 * here changes what the game gets. */
#ifndef RIFTWII_GC_PADDIAG_H
#define RIFTWII_GC_PADDIAG_H

#include <stdint.h>

#define GC_PD_PORTS 4u

/* gc_pad_port_diag.owner: what RiftWii thinks plays on the port. */
#define GC_PD_OWNER_NONE 0u
#define GC_PD_OWNER_PAD 1u      /* a GameCube controller: the game's own PADRead found one */
#define GC_PD_OWNER_ADAPTER 2u  /* the GameCube adapter's controller (gc_gcad.c) */
#define GC_PD_OWNER_REMOTE 3u   /* a Wii Remote alone (held sideways) */
#define GC_PD_OWNER_NUNCHUK 4u
#define GC_PD_OWNER_CLASSIC 5u
#define GC_PD_OWNER_PRO 6u      /* a Wii U Pro Controller */

struct gc_pad_port_diag {
    uint32_t raw_none;       /* PADReads where the game's own read gave this port err 0 (PAD_ERR_NONE) */
    uint32_t raw_absent;     /* ... err -1 (PAD_ERR_NO_CONTROLLER) */
    uint32_t raw_other;      /* ... any other err (-2 NOT_READY, -3 TRANSFER) */
    uint32_t final_nonzero;  /* PADReads that left the game a non-zero button word on this port */
    uint32_t changed;        /* calls that found this port's button word or err changed since RiftWii wrote it
                              * (sticks and triggers are left out: the SDK's PADClamp rewrites those) */
    uint32_t remote_fills;   /* times a Wii Remote was written into this port */
    uint32_t remote_nonzero; /* ... with a non-zero button word */
    uint32_t src_nonzero;    /* ... where the remote's own report had something held (before the map) */
    uint32_t last_src;       /* core 0 << 24 | core 1 << 16 | extension 4 << 8 | extension 5, at the last such fill */
    uint16_t last_buttons;   /* the last non-zero button word the game was left with */
    uint8_t last_stick[4];   /* stick x, y and C-stick x, y of that PADStatus */
    uint8_t last_trigger[2]; /* L and R of that PADStatus */
    uint8_t raw_err;         /* the last err the game's own read gave (0xFF: no controller) */
    uint8_t out_err;         /* the last err the game was left with, after RiftWii's fills */
    uint8_t owner;           /* GC_PD_OWNER_* at the last read */
    uint8_t slot;            /* the Wii Remote slot that played here at the last read, 0xFF: none */
};

struct gc_pad_diag {
    uint32_t calls;          /* PADReads seen before the original ran (gc_on_pad_before) */
    uint32_t first_tbu;      /* the time base (upper word) at the first call */
    uint32_t first_tb;       /* ... and its lower word */
    uint32_t last_tb;        /* ... at the last call */
    uint32_t gap_max;        /* the longest time base gap between two calls */
    uint32_t last_status;    /* the PADStatus address of the last call */
    uint32_t status_moves;   /* calls whose address differed from the call before */
    uint32_t unusable;       /* calls whose address was not usable (RiftWii did not touch them) */
    uint32_t last_result;    /* the value the game's PADRead returned (its mask of ports without a controller) */
    uint32_t result_mismatch; /* PADReads whose result named a port RiftWii had filled with a controller */
    uint32_t fills;          /* PADReads where at least one Wii Remote was ready to play */
    uint32_t report_tb;      /* the time base when the newest data report was parsed */
    uint32_t age_last;       /* a call's time base minus report_tb: how old the newest report was */
    uint32_t age_max;
    uint32_t console_id;     /* the console's chip ID word (0xCD8005A0) as the loader read it, 0: not read */
    uint32_t overwrite_ours; /* the first non-zero PADStatus RiftWii wrote that was changed before the next call: */
    uint32_t overwrite_seen; /* port << 24 | err << 16 | buttons, as written, then err << 16 | buttons, as found */
    uint8_t written[12 * GC_PD_PORTS]; /* the PADStatus[4] as RiftWii left it, for the next call to compare */
    uint8_t written_valid;
    struct gc_pad_port_diag port[GC_PD_PORTS];
};

#endif
