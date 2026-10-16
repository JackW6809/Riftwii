/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube disc blob's C part (gc_rt.h). Same rules as the other
 * blobs: no globals, no literals that need relocations, no library
 * calls; all state is in the context the trampolines hand over. */
#include "gc_rt.h"
#include "gc_bt.h"
#include "gc_card.h"

/* Hollywood's IPC registers, uncached (wiibrew "Hardware/IPC"): the
 * request's physical address, the control bits, IOS's reply. */
#define GC_IPC_PPCMSG 0xCD000000u
#define GC_IPC_PPCCTRL 0xCD000004u
#define GC_IPC_ARMMSG 0xCD000008u
#define GC_IPC_X1 0x01u  /* a request is waiting for IOS */
#define GC_IPC_Y2 0x02u  /* IOS took it (write 1 to clear) */
#define GC_IPC_Y1 0x04u  /* IOS replied (write 1 to clear) */
#define GC_IPC_X2 0x08u  /* the reply is read: IOS may send the next */

#define GC_IOS_IOCTLV 7u
#define GC_SDIO_SENDCMD 7u
#define GC_SD_READMULTIBLOCK 0x12u
#define GC_SD_CMDTYPE_AC 3u
#define GC_SD_RESPONSE_R1 1u
#define GC_SECTOR 512u
/* SENDCMD's reply: exactly 16 bytes (three response words and the
 * auto-CMD12 one). IOS on a Wii refuses any other length with -4;
 * Dolphin does not check. */
#define GC_SD_RESPONSE_BYTES 16u
#define GC_TRACE_SKIPPED (-3000)  /* no trace, or the IPC buffers were in use */

static uint32_t gc_read32(uint32_t address) { return *(volatile uint32_t*)(uintptr_t)address; }
static void gc_write32(uint32_t address, uint32_t value) { *(volatile uint32_t*)(uintptr_t)address = value; }

static uint32_t gc_physical(uint32_t address) { return address & 0x3FFFFFFFu; }

void gc_flush(uint32_t address, uint32_t length) {
    uint32_t a = address & ~31u;
    const uint32_t end = address + length;
    for (; a < end; a += 32) __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

void gc_invalidate(uint32_t address, uint32_t length) {
    uint32_t a = address & ~31u;
    const uint32_t end = address + length;
    for (; a < end; a += 32) __asm__ volatile("dcbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static void gc_copy(uint8_t* dst, const uint8_t* src, uint32_t n) {
    while (n--) *dst++ = *src++;
}

static void gc_zero(uint8_t* dst, uint32_t n) {
    while (n--) *dst++ = 0;
}

/* The guard (gc_rt.h): the game's bytes at guard_addr put back wherever
 * IOS wrote over them, in memory and out of both caches. */
#define GC_GUARD_WORDS 64u
void gc_guard(struct gc_rt_context* ctx) {
    uint32_t i;
    if (ctx->guard_addr == 0) return;
    for (i = 0; i < GC_GUARD_WORDS; ++i) {
        const uint32_t cached = ctx->guard_addr + 4 * i;
        const uint32_t uncached = gc_physical(cached) | 0xC0000000u;
        const uint32_t now = gc_read32(uncached);
        if (now == ctx->guard_copy[i]) continue;
        if (ctx->guard_hits == 0) {
            ctx->guard_first_request = ctx->sd_requests;
            ctx->guard_first_offset = 4 * i;
            ctx->guard_first_value = now;
        }
        ctx->guard_hits++;
        gc_write32(uncached, ctx->guard_copy[i]);
        __asm__ volatile("dcbi 0,%0\n\tsync\n\ticbi 0,%0\n\tisync" : : "r"(cached) : "memory");
    }
}

/* The reply IOS has ready (the request's physical address), or 0. */
uint32_t gc_ipc_take(void) {
    uint32_t reply;
    if ((gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y1) == 0) return 0;
    reply = gc_read32(GC_IPC_ARMMSG);
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y1);
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_X2);
    return reply;
}

/* A request to IOS (its physical address): sent, and taken by IOS. IOS
 * takes no new request while a reply waits to be acknowledged, and the
 * Wii Remotes' reads (gc_bt.c) finish whenever a remote sends something:
 * a reply waiting meanwhile goes to them, or the send never completes
 * (Luigi's Mansion in Dolphin: every disc read timed out, the control
 * register at X1 | Y1). Every reply but a pending request's own is
 * theirs: disc reads and trace writes run one at a time. */
int32_t gc_ipc_send(uint32_t phys, struct gc_bt* bt) {
    uint32_t polls;
    gc_write32(GC_IPC_PPCMSG, phys);
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_X1);
    for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y2) == 0; ++polls) {
        const uint32_t reply = gc_ipc_take();
        if (reply != 0 && bt) gc_bt_on_reply(bt, reply);
        if (polls == GC_IPC_TIMEOUT) return -1000;
    }
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y2);
    return 0;
}

/* A reply that is not the one being waited for: the Wii Remotes' reads
 * (gc_bt.c), or something left from the menu (dropped). */
static void gc_ipc_other(struct gc_rt_context* ctx, uint32_t reply) {
    if (ctx->bt) gc_bt_on_reply((struct gc_bt*)(uintptr_t)ctx->bt, reply);
}

/* One ioctlv to IOS and its reply, by polling (the Wii Remotes' reads may
 * be in flight meanwhile; their replies go to gc_bt.c). Returns IOS's
 * result, or -1000 when IOS does not answer. */
static int32_t gc_ioctlv(struct gc_rt_context* ctx, uint32_t fd, uint32_t ioctl, uint32_t in_count,
                         uint32_t out_count) {
    uint32_t* rq = ctx->ipc;
    const uint32_t phys = gc_physical((uint32_t)(uintptr_t)rq);
    uint32_t i;
    uint32_t polls;
    for (i = 0; i < 16; ++i) rq[i] = 0;
    rq[0] = GC_IOS_IOCTLV;
    rq[2] = fd;
    rq[3] = ioctl;
    rq[4] = in_count;
    rq[5] = out_count;
    rq[6] = gc_physical((uint32_t)(uintptr_t)ctx->vec);
    gc_flush((uint32_t)(uintptr_t)rq, sizeof(ctx->ipc));
    if (gc_ipc_send(phys, (struct gc_bt*)(uintptr_t)ctx->bt) != 0) return -1000;
    for (;;) {
        /* The launch's reply never comes when IOS takes it (it reloads and
         * the PPC is reset): with ipc_no_timeout the wait has no end, as a
         * timeout would give the game back to a dead IOS; a refusal does
         * come back as a reply. */
        for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y1) == 0; ++polls)
            if (polls == GC_IPC_TIMEOUT && !ctx->ipc_no_timeout) return -1000;
        {
            const uint32_t reply = gc_read32(GC_IPC_ARMMSG);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y1);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_X2);
            if (reply == phys) break;
            gc_ipc_other(ctx, reply);
        }
    }
    gc_invalidate((uint32_t)(uintptr_t)rq, sizeof(ctx->ipc));
    gc_guard(ctx);
    return (int32_t)rq[1];
}

/* CMD18: `count` sectors from `sector` into `target` (32-byte aligned). */
static int gc_sd_read(struct gc_rt_context* ctx, uint32_t sector, uint32_t count, uint32_t target) {
    uint32_t* q = ctx->request;
    int32_t result;
    q[0] = GC_SD_READMULTIBLOCK;
    q[1] = GC_SD_CMDTYPE_AC;
    q[2] = GC_SD_RESPONSE_R1;
    q[3] = ctx->sdhc ? sector : sector * GC_SECTOR;
    q[4] = count;
    q[5] = GC_SECTOR;
    q[6] = target;
    q[7] = 1;  /* DMA */
    q[8] = 0;
    ctx->vec[0] = gc_physical((uint32_t)(uintptr_t)q);
    ctx->vec[1] = 36;
    ctx->vec[2] = gc_physical(target);
    ctx->vec[3] = count * GC_SECTOR;
    ctx->vec[4] = gc_physical((uint32_t)(uintptr_t)ctx->response);
    ctx->vec[5] = GC_SD_RESPONSE_BYTES;
    gc_flush((uint32_t)(uintptr_t)q, 36);
    gc_flush((uint32_t)(uintptr_t)ctx->vec, sizeof(ctx->vec));
    gc_flush(target, count * GC_SECTOR);  /* no dirty lines left to land on the DMA's bytes */
    ctx->sd_requests++;
    result = gc_ioctlv(ctx, ctx->sdio_fd, GC_SDIO_SENDCMD, 2, 1);
    gc_invalidate(target, count * GC_SECTOR);
    if (result < 0) {
        ctx->failures++;
        ctx->last_error = result;
        return 0;
    }
    return 1;
}

/* `length` bytes of a card run from byte `at` of it into `dst`. Whole
 * sectors go straight into the game's buffer when it is aligned for the
 * DMA; the rest through the bounce buffer. */
static int gc_read_sd_run(struct gc_rt_context* ctx, const struct gc_rt_run* run, uint32_t at, uint8_t* dst,
                          uint32_t length) {
    while (length != 0) {
        const uint32_t byte = run->skip + at;  /* from the start of sector `source` */
        const uint32_t sector = run->source + byte / GC_SECTOR;
        const uint32_t into = byte % GC_SECTOR;
        if (into == 0 && length >= GC_SECTOR && ((uint32_t)(uintptr_t)dst & 31u) == 0) {
            uint32_t count = length / GC_SECTOR;
            if (count > 128) count = 128;
            if (!gc_sd_read(ctx, sector, count, (uint32_t)(uintptr_t)dst)) return 0;
            dst += count * GC_SECTOR;
            at += count * GC_SECTOR;
            length -= count * GC_SECTOR;
        } else {
            uint32_t want = into + length;
            uint32_t count;
            uint32_t got;
            if (want > ctx->bounce_bytes) want = ctx->bounce_bytes;
            count = (want + GC_SECTOR - 1) / GC_SECTOR;
            if (!gc_sd_read(ctx, sector, count, ctx->bounce)) return 0;
            got = count * GC_SECTOR - into;
            if (got > length) got = length;
            gc_copy(dst, (const uint8_t*)(uintptr_t)(ctx->bounce + into), got);
            dst += got;
            at += got;
            length -= got;
        }
    }
    return 1;
}

/* The game's view of the disc, `length` bytes from `offset` into `dst`. */
static int gc_read_disc_unlocked(struct gc_rt_context* ctx, uint8_t* dst, uint32_t length, uint32_t offset) {
    const struct gc_rt_run* runs = (const struct gc_rt_run*)(uintptr_t)ctx->runs;
    const uint32_t start = (uint32_t)(uintptr_t)dst;
    const uint32_t total = length;
    uint32_t i = 0;
    while (length != 0) {
        uint32_t at;
        uint32_t n;
        while (i < ctx->run_count && runs[i].disc + runs[i].length <= offset) ++i;
        if (i == ctx->run_count || runs[i].disc > offset) return 0;
        at = offset - runs[i].disc;
        n = runs[i].length - at;
        if (n > length) n = length;
        if (runs[i].kind == GC_RUN_SD) {
            if (!gc_read_sd_run(ctx, &runs[i], at, dst, n)) return 0;
        } else if (runs[i].kind == GC_RUN_MEM) {
            gc_copy(dst, (const uint8_t*)(uintptr_t)(runs[i].source + at), n);
        } else {
            gc_zero(dst, n);
        }
        dst += n;
        offset += n;
        length -= n;
    }
    /* As the DI's DMA leaves it: in memory, not only in the cache. */
    gc_flush(start, total);
    return 1;
}

/* ctx->busy: a read is using the IPC buffers. The AI interrupt's stream
 * refill finds it set when it interrupted the game's own read, and waits
 * for the next buffer instead. */
static int gc_read_disc(struct gc_rt_context* ctx, uint8_t* dst, uint32_t length, uint32_t offset) {
    int ok;
    if (ctx->busy) return 0;
    ctx->busy = 1;
    ok = gc_read_disc_unlocked(ctx, dst, length, offset);
    ctx->busy = 0;
    return ok;
}

uint32_t gc_irq_off(void) {
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr & ~0x8000u) : "memory");
    return msr;
}
void gc_irq_restore(uint32_t msr) { __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory"); }

/* One ioctl (no vectors), polled like gc_ioctlv: CMD13 takes this form. */
static int32_t gc_ioctl(struct gc_rt_context* ctx, uint32_t fd, uint32_t ioctl, uint32_t in, uint32_t in_len,
                        uint32_t out, uint32_t out_len) {
    uint32_t* rq = ctx->ipc;
    const uint32_t phys = gc_physical((uint32_t)(uintptr_t)rq);
    uint32_t i;
    uint32_t polls;
    for (i = 0; i < 16; ++i) rq[i] = 0;
    rq[0] = 6;  /* IOS_IOCTL */
    rq[2] = fd;
    rq[3] = ioctl;
    rq[4] = gc_physical(in);
    rq[5] = in_len;
    rq[6] = gc_physical(out);
    rq[7] = out_len;
    gc_flush(in, in_len);
    gc_flush(out, out_len);
    gc_flush((uint32_t)(uintptr_t)rq, sizeof(ctx->ipc));
    if (gc_ipc_send(phys, (struct gc_bt*)(uintptr_t)ctx->bt) != 0) return -1000;
    for (;;) {
        /* The launch's reply never comes when IOS takes it (it reloads and
         * the PPC is reset): with ipc_no_timeout the wait has no end, as a
         * timeout would give the game back to a dead IOS; a refusal does
         * come back as a reply. */
        for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y1) == 0; ++polls)
            if (polls == GC_IPC_TIMEOUT && !ctx->ipc_no_timeout) return -1000;
        {
            const uint32_t reply = gc_read32(GC_IPC_ARMMSG);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y1);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_X2);
            if (reply == phys) break;
            gc_ipc_other(ctx, reply);
        }
    }
    gc_invalidate((uint32_t)(uintptr_t)rq, sizeof(ctx->ipc));
    gc_invalidate(out, out_len);
    gc_guard(ctx);
    return (int32_t)rq[1];
}

/* `count` sectors (at most 16) from `buf` (32-byte aligned) to the card
 * at `sector`: CMD25, then CMD13 until the card has written them. The
 * caller holds ctx->busy. */
static int32_t gc_sd_write_locked(struct gc_rt_context* ctx, uint32_t sector, uint32_t count, uint32_t buf) {
    uint32_t* q = ctx->request;
    int32_t result;
    uint32_t i;
    q[0] = 0x19u;
    q[1] = GC_SD_CMDTYPE_AC;
    q[2] = GC_SD_RESPONSE_R1;
    q[3] = ctx->sdhc ? sector : sector * GC_SECTOR;
    q[4] = count;
    q[5] = GC_SECTOR;
    q[6] = buf;
    q[7] = 1;
    q[8] = 0;
    ctx->vec[0] = gc_physical((uint32_t)(uintptr_t)q);
    ctx->vec[1] = 36;
    ctx->vec[2] = gc_physical(buf);
    ctx->vec[3] = count * GC_SECTOR;
    ctx->vec[4] = gc_physical((uint32_t)(uintptr_t)ctx->response);
    ctx->vec[5] = GC_SD_RESPONSE_BYTES;
    gc_flush((uint32_t)(uintptr_t)q, 36);
    gc_flush((uint32_t)(uintptr_t)ctx->vec, sizeof(ctx->vec));
    gc_flush(buf, count * GC_SECTOR);
    ctx->sd_requests++;
    result = gc_ioctlv(ctx, ctx->sdio_fd, GC_SDIO_SENDCMD, 2, 1);
    if (result >= 0 && ctx->card_rca != 0) {
        for (i = 0; i < 8192; ++i) {
            uint32_t state;
            q[0] = 0x0Du;
            q[1] = GC_SD_CMDTYPE_AC;
            q[2] = GC_SD_RESPONSE_R1;
            q[3] = ctx->card_rca << 16;
            q[4] = q[5] = q[6] = q[7] = q[8] = 0;
            ctx->response[0] = 0;
            if (gc_ioctl(ctx, ctx->sdio_fd, GC_SDIO_SENDCMD, (uint32_t)(uintptr_t)q, 36,
                         (uint32_t)(uintptr_t)ctx->response, 16) < 0)
                break;
            state = (ctx->response[0] >> 9) & 0xFu;
            if (state != 6 && state != 7) break;  /* left receive-data and programming */
        }
    }
    return result;
}

int32_t gc_sd_write(struct gc_rt_context* ctx, uint32_t sector, uint32_t count, uint32_t buf) {
    int32_t result;
    const uint32_t msr = gc_irq_off();
    if (ctx->busy) {
        gc_irq_restore(msr);
        return GC_TRACE_SKIPPED;
    }
    ctx->busy = 1;
    result = gc_sd_write_locked(ctx, sector, count, buf);
    ctx->busy = 0;
    gc_irq_restore(msr);
    return result;
}

/* Record `seq`'s sector: the first records stay (how the game started),
 * the last GC_TRACE_RING go round. */
static uint32_t gc_trace_slot(const struct gc_rt_context* ctx, uint32_t seq) {
    const uint32_t kept = ctx->trace_count > 2 * GC_TRACE_RING ? ctx->trace_count - GC_TRACE_RING : ctx->trace_count / 2;
    const uint32_t ring = ctx->trace_count - kept;
    if (seq <= kept) return ctx->trace_sector + seq - 1;
    return ctx->trace_sector + kept + (seq - kept - 1) % ring;
}

/* One record of the trace (gc_rt.h), with interrupts held off so it
 * cannot meet a read on the card; skipped while a read is going on. */
static int32_t gc_trace(struct gc_rt_context* ctx, uint32_t where) {
    int32_t result;
    uint32_t* w = (uint32_t*)(uintptr_t)ctx->trace_buf;
    const struct gc_rt_stream* st = (const struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t msr, tbu, tbl, i, sector;
    if (ctx->trace_sector == 0 || ctx->trace_count == 0 || w == 0) return GC_TRACE_SKIPPED;
    msr = gc_irq_off();
    if (ctx->busy) {
        gc_irq_restore(msr);
        return GC_TRACE_SKIPPED;
    }
    ctx->busy = 1;
    __asm__ volatile("mftbu %0" : "=r"(tbu));
    __asm__ volatile("mftb %0" : "=r"(tbl));
    for (i = 0; i < 128; ++i) w[i] = 0;
    w[GC_TR_MAGIC] = GC_TRACE_MAGIC;
    w[GC_TR_SEQ] = ++ctx->trace_seq;
    w[GC_TR_WHERE] = where;
    w[GC_TR_TBU] = tbu;
    w[GC_TR_TBL] = tbl;
    w[GC_TR_COMMANDS] = ctx->commands;
    w[GC_TR_READS] = ctx->reads;
    w[GC_TR_READ_BYTES] = ctx->read_bytes;
    w[GC_TR_SD_REQUESTS] = ctx->sd_requests;
    w[GC_TR_FAILURES] = ctx->failures;
    w[GC_TR_LAST_ERROR] = (uint32_t)ctx->last_error;
    w[GC_TR_LAST_OFFSET] = ctx->last_offset;
    w[GC_TR_PAD_READS] = ctx->pad_reads;
    if (st) {
        w[GC_TR_STREAM_PLAYING] = st->playing;
        w[GC_TR_STREAM_MIXES] = st->mixes;
        w[GC_TR_STREAM_REFILLS] = st->refills;
        w[GC_TR_STREAM_STARVED] = st->starved;
        w[GC_TR_STREAM_FETCH_BUSY] = st->fetch_busy;
        w[GC_TR_STREAM_BUFFERED] = st->fetched - st->consumed;
    }
    w[GC_TR_AI_DMAS] = ctx->ai_dmas;
    w[GC_TR_AR_DMAS] = ctx->ar_dmas;
    for (i = 0; i < 4; ++i) w[GC_TR_LAST_AR_TYPE + i] = ctx->last_ar[i];
    if (ctx->bt) {
        const struct gc_bt* bt = (const struct gc_bt*)(uintptr_t)ctx->bt;
        w[GC_TR_BT_STAGE] = bt->stage;
        w[GC_TR_BT_ERROR] = (uint32_t)bt->last_error;
        w[GC_TR_BT_EVENTS] = bt->event_count;
        w[GC_TR_BT_ACL] = bt->acl_count;
        w[GC_TR_BT_LAST_EVENT] = bt->last_event;
        w[GC_TR_BT_KEYS] = bt->key_count << 16 | bt->connects;
        w[GC_TR_BT_REMOTES] = (uint32_t)bt->remotes[0].chans << 24 | (uint32_t)bt->remotes[0].ready << 16 |
                              bt->remotes[0].ext_step << 8 | bt->remotes[0].ext_kind;
        w[GC_TR_BT_BUTTONS] = (uint32_t)bt->remotes[0].core[0] << 8 | bt->remotes[0].core[1];
        w[GC_TR_BT_EVENT_LENGTHS] = bt->event_lengths;
        gc_copy((uint8_t*)&w[GC_TR_BT_EVENT_LOG], &bt->event_log[0][0], 32);
    }
    if (ctx->card) {
        const struct gc_card* card = (const struct gc_card*)(uintptr_t)ctx->card;
        w[GC_TR_CARD_READS] = card->reads;
        w[GC_TR_CARD_WRITES] = card->programs << 16 | (card->erases & 0xFFFFu);
        w[GC_TR_CARD_FLUSHES] = card->flushes;
        w[GC_TR_CARD_DIRTY] = card->dirty_count;
        w[GC_TR_CARD_ERROR] = (uint32_t)card->flush_error;
        w[GC_TR_CARD_LAST] = card->last_cmd << 24 | (card->activity & 0xFFFFFFu);
    }
    w[GC_TR_VI_CONFIGURES] = ctx->vi_configures;
    w[GC_TR_VI_MODE] = ctx->vi_mode;
    w[GC_TR_PROJECTIONS] = ctx->projections;
    w[GC_TR_ORTHOGRAPHICS] = ctx->orthographics;
    for (i = 0; i < GC_PROJECTION_CALLERS * 4; ++i) w[GC_TR_PROJECTION_CALLERS + i] = ctx->projection_callers[i];
    w[GC_TR_PENDING_CB] = ctx->pending_cb;
    w[GC_TR_ALARMS] = ctx->alarms;
    w[GC_TR_BEATS] = ctx->beats;
    w[GC_TR_BEAT_PC] = ctx->beat_pc;
    w[GC_TR_BEAT_LR] = ctx->beat_lr;
    w[GC_TR_BEAT_MSR] = ctx->beat_msr;
    w[GC_TR_EXCEPTION] = ctx->exception;
    w[GC_TR_DSISR] = ctx->dsisr;
    w[GC_TR_DAR] = ctx->dar;
    for (i = 0; i < 5; ++i) w[GC_TR_CRASH_R1 + i] = ctx->crash_gpr[i];
    for (i = 0; i < 4; ++i) w[GC_TR_CRASH_CHAIN + i] = ctx->crash_chain[i];
    w[GC_TR_CRASH_WORD] = ctx->crash_word;
    w[GC_TR_GUARD_HITS] = ctx->guard_hits;
    w[GC_TR_GUARD_FIRST_REQUEST] = ctx->guard_first_request;
    w[GC_TR_GUARD_FIRST_OFFSET] = ctx->guard_first_offset;
    w[GC_TR_GUARD_FIRST_VALUE] = ctx->guard_first_value;
    /* Interrupts: the PI's cause and mask, and Hollywood's lines to the
     * CPU (Wii-only; a GameCube game cannot clear them). */
    w[GC_TR_PI_CAUSE] = gc_read32(0xCC003000u);
    w[GC_TR_PI_MASK] = gc_read32(0xCC003004u);
    w[GC_TR_HW_IRQ_FLAG] = gc_read32(0xCD000030u);
    w[GC_TR_HW_IRQ_MASK] = gc_read32(0xCD000034u);
    sector = gc_trace_slot(ctx, ctx->trace_seq);
    result = gc_sd_write_locked(ctx, sector, 1, ctx->trace_buf);
    ctx->busy = 0;
    gc_irq_restore(msr);
    return result;
}

/* The DI's immediate register, where the SDK reads a status reply. */
static void gc_set_imm(struct gc_rt_context* ctx, uint32_t value) {
    if (ctx->di_regs != 0) *(volatile uint32_t*)(uintptr_t)(ctx->di_regs + 0x20) = value;
}

/* The callback runs from an OSAlarm a moment from now, where the DI
 * interrupt would have run it. */
typedef void (*gc_set_alarm_fn)(void* alarm, uint64_t ticks, uint32_t handler);
static void gc_complete_after(struct gc_rt_context* ctx, uint32_t cb, uint32_t type, uint32_t ticks) {
    gc_set_alarm_fn set_alarm = (gc_set_alarm_fn)(uintptr_t)ctx->set_alarm;
    ctx->pending_cb = cb;
    ctx->pending_type = type;
    set_alarm(ctx->alarm, ticks < ctx->delay_ticks ? ctx->delay_ticks : ticks, ctx->alarm_entry);
}
static void gc_complete(struct gc_rt_context* ctx, uint32_t cb, uint32_t type) { gc_complete_after(ctx, cb, type, 0); }

/* How long a GameCube drive takes for a read, in timebase ticks (the Wii's
 * bus clock / 4, as the game is told): the seek, from nothing for a read
 * that follows the last one to about 105 ms across the whole disc, then
 * the transfer, the disc turning at a constant speed so the rate grows
 * from about 2 MB/s at its start to about 3 MB/s at its end. A read
 * served at the SD card's speed finished in microseconds, and some games
 * count on the drive's time (Nintendont emulates it too). A rough model,
 * not a measured one. */
#define GC_TICKS_PER_US_X4 243u  /* 60.75 ticks a microsecond */
static uint32_t gc_drive_ticks(struct gc_rt_context* ctx, uint32_t offset, uint32_t length) {
    const uint32_t distance = offset > ctx->last_read_end ? offset - ctx->last_read_end : ctx->last_read_end - offset;
    uint32_t seek_us = 0, rate_x10, transfer_us, us;
    if (distance >= 0x10000u) {
        seek_us = 20000u + (distance >> 14);
        if (seek_us > 105000u) seek_us = 105000u;
    }
    rate_x10 = 20u + (offset >> 27);  /* bytes a microsecond, times 10: 20 at the start, 30 at 1.4 GB */
    transfer_us = (length / rate_x10) * 10u + (length % rate_x10) * 10u / rate_x10;
    us = seek_us + transfer_us;
    if (us > 4000000u) us = 4000000u;
    return us / 4u * GC_TICKS_PER_US_X4 + (us % 4u) * GC_TICKS_PER_US_X4 / 4u;
}

/* The heartbeat (gc_rt.h): once more, half a second from now. */
static void gc_beat_arm(struct gc_rt_context* ctx) {
    gc_set_alarm_fn set_alarm = (gc_set_alarm_fn)(uintptr_t)ctx->set_alarm;
    set_alarm(ctx->beat_alarm, ctx->beat_ticks, ctx->alarm_entry);
}

/* Every DVDLow command: the first starts the heartbeat (the game's OS is
 * up by then, its alarms work), and the first few that are not reads get
 * a trace record too (reads write their own). */
static void gc_command_seen(struct gc_rt_context* ctx, int traced) {
    ctx->commands++;
    if (ctx->commands == 1 && ctx->beat_ticks != 0) gc_beat_arm(ctx);
    if (traced && ctx->commands <= 32) gc_trace(ctx, GC_TRACE_COMMAND);
}

static void gc_stream_fetch(struct gc_rt_context* ctx);

uint32_t gc_on_read(struct gc_rt_context* ctx, uint32_t addr, uint32_t length, uint32_t offset, uint32_t cb) {
    gc_command_seen(ctx, 0);
    ctx->reads++;
    ctx->read_bytes += length;
    ctx->last_offset = offset;
    {
        uint32_t start, end, wait = 0;
        int ok;
        __asm__ volatile("mftb %0" : "=r"(start));
        ok = gc_read_disc(ctx, (uint8_t*)(uintptr_t)addr, length, offset);
        __asm__ volatile("mftb %0" : "=r"(end));
        if (ctx->drive_timing) {
            /* What the drive would still need after the time the card took. */
            const uint32_t drive = gc_drive_ticks(ctx, offset, length);
            const uint32_t spent = end - start;
            wait = drive > spent ? drive - spent : 0;
        }
        ctx->last_read_end = offset + length;
        gc_complete_after(ctx, cb, ok ? GC_DI_TC : GC_DI_DE, wait);
    }
    gc_stream_fetch(ctx);  /* the card is free again: the music ring's top-up */
    if (ctx->reads <= 32 || (ctx->reads & 63) == 0) gc_trace(ctx, GC_TRACE_READ);
    return 1;
}

uint32_t gc_on_disk_id(struct gc_rt_context* ctx, uint32_t id, uint32_t cb) {
    gc_command_seen(ctx, 1);
    gc_complete(ctx, cb, gc_read_disc(ctx, (uint8_t*)(uintptr_t)id, 0x20, 0) ? GC_DI_TC : GC_DI_DE);
    return 1;
}

/* DVDDriveInfo: revision, device code, release date, then padding. */
uint32_t gc_on_inquiry(struct gc_rt_context* ctx, uint32_t info, uint32_t cb) {
    uint32_t* words = (uint32_t*)(uintptr_t)info;
    uint32_t i;
    gc_command_seen(ctx, 1);
    for (i = 0; i < 8; ++i) words[i] = 0;
    words[0] = 0x00020201u;
    words[1] = 0x20020402u;
    gc_flush(info, 0x20);
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

uint32_t gc_on_done(struct gc_rt_context* ctx, uint32_t cb) {
    gc_command_seen(ctx, 1);
    gc_set_imm(ctx, 0);  /* RequestError: no error */
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

typedef void (*gc_callback_fn)(uint32_t type);
/* An OSContext's saved LR, SRR0 and SRR1 (the SDK's layout). */
#define GC_OSCONTEXT_LR 0x84u
#define GC_OSCONTEXT_SRR0 0x198u
#define GC_OSCONTEXT_SRR1 0x19Cu

void gc_on_alarm(struct gc_rt_context* ctx, uint32_t alarm, uint32_t os_context) {
    const uint32_t cb = ctx->pending_cb;
    if (alarm == (uint32_t)(uintptr_t)ctx->beat_alarm) {
        ctx->beats++;
        if (os_context != 0) {
            ctx->beat_pc = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_SRR0);
            ctx->beat_lr = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_LR);
            ctx->beat_msr = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_SRR1);
        }
        gc_guard(ctx);
        if (ctx->card) gc_card_flush(ctx, (struct gc_card*)(uintptr_t)ctx->card, 0);
        gc_trace(ctx, GC_TRACE_BEAT);
        gc_beat_arm(ctx);
        return;
    }
    if (ctx->card && alarm == (uint32_t)(uintptr_t)((struct gc_card*)(uintptr_t)ctx->card)->alarm) {
        gc_card_on_alarm(ctx, (struct gc_card*)(uintptr_t)ctx->card, os_context);
        return;
    }
    ctx->alarms++;
    ctx->pending_cb = 0;
    if (cb != 0) ((gc_callback_fn)(uintptr_t)cb)(ctx->pending_type);
}

/* ---- Disc audio streaming ------------------------------------------ */

/* The audio interface where a Wii in Wii mode has it (0xCC006C00 on a
 * GameCube; the loader moves the game's own accesses too). */
#define GC_AI_CR 0xCD006C00u
#define GC_AI_VR 0xCD006C04u
#define GC_AI_PSTAT 0x01u   /* the stream plays */
#define GC_AI_32K 0x40u     /* the DMA (the game's own audio) runs at 32 kHz, as the game sets it */
#define GC_ADP_BLOCK 32u    /* 28 stereo samples */

static void gc_stream_reset(struct gc_rt_stream* st) {
    st->hist[0] = st->hist[1] = st->hist[2] = st->hist[3] = 0;
    st->sample = 28;
    st->phase = 0;
    st->fetched = 0;
    st->consumed = 0;
    st->ended = 0;
}

/* One top-up of the ring from the card: up to GC_STREAM_CHUNK bytes,
 * never across the ring's end. Called only from the game's own context
 * (see gc_rt_stream); the card being in use puts it off to the next
 * call. Moves on to the queued stream when one ends. */
static void gc_stream_fetch(struct gc_rt_context* ctx) {
    struct gc_rt_stream* st = (struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t buffered;
    uint32_t free_bytes;
    uint32_t at;
    uint32_t n;
    if (st == 0 || !st->playing || st->ended) return;
    if (st->pos >= st->start + st->length) {
        if (st->next_length == 0) {
            st->ended = 1;
            return;
        }
        st->start = st->next_start;
        st->length = st->next_length;
        st->pos = st->start;
    }
    buffered = st->fetched - st->consumed;
    free_bytes = st->buf_bytes - buffered;
    /* Fewer, larger reads: each is one IOS request, a millisecond or more
     * on a Wii. Whole sectors keep them going straight into the ring (a
     * stream starts on a sector, as the drive's own blocks do). */
    if (free_bytes < GC_STREAM_CHUNK / 2) return;
    at = st->fetched % st->buf_bytes;
    n = GC_STREAM_CHUNK;
    if (n > free_bytes) n = free_bytes;
    if (n > st->buf_bytes - at) n = st->buf_bytes - at;
    n &= ~(GC_SECTOR - 1);
    if (n > st->start + st->length - st->pos) n = (st->start + st->length - st->pos) & ~(GC_ADP_BLOCK - 1);
    if (n == 0) return;
    if (!gc_read_disc(ctx, (uint8_t*)(uintptr_t)(st->buf + at), n, st->pos)) {
        st->fetch_busy++;
        return;
    }
    st->pos += n;
    st->fetched += n;  /* last: the decoder may take these bytes from now on */
    st->refills++;
}

/* E1: set the stream (or the next one), or cancel it. */
uint32_t gc_on_audio_stream(struct gc_rt_context* ctx, uint32_t sub, uint32_t length, uint32_t offset, uint32_t cb) {
    struct gc_rt_stream* st = (struct gc_rt_stream*)(uintptr_t)ctx->stream;
    gc_command_seen(ctx, 1);
    if (st != 0) {
        if (sub & 0x00010000u) {
            st->playing = 0;  /* cancel at once */
        } else if (length != 0) {
            if (!st->playing) {
                st->start = offset;
                st->length = length;
                st->pos = offset;
                gc_stream_reset(st);
                st->playing = 1;
                /* A good start in the ring before the AI asks for any. */
                gc_stream_fetch(ctx);
                gc_stream_fetch(ctx);
            }
            st->next_start = offset;  /* the drive loops a stream until told otherwise */
            st->next_length = length;
        } else {
            st->next_length = 0;  /* stop at its end */
        }
    }
    gc_set_imm(ctx, 0);
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

/* E2: the stream's state, address, start or length, in the immediate
 * register (addresses in 4-byte words, as the drive gives them). */
uint32_t gc_on_audio_status(struct gc_rt_context* ctx, uint32_t sub, uint32_t cb) {
    const struct gc_rt_stream* st = (const struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t value = 0;
    gc_command_seen(ctx, 1);
    if (st != 0) {
        const uint32_t which = (sub >> 16) & 0xFFu;
        const uint32_t played = st->pos - (st->fetched - st->consumed);
        if (which == 0) value = st->playing ? 1u : 0u;
        else if (which == 1) value = played >> 2;
        else if (which == 2) value = st->start >> 2;
        else if (which == 3) value = st->length;
    }
    gc_set_imm(ctx, value);
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

static int32_t gc_clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* One 4-bit sample of the drive's ADPCM: the filter in the header's top
 * nibble, the shift in its bottom one (the format as documented in yagcd
 * and implemented in Dolphin's StreamADPCM). */
static int16_t gc_adp_sample(int32_t nibble, uint32_t header, int32_t* hist1, int32_t* hist2) {
    int32_t predict = 0;
    int32_t cur;
    switch (header >> 4) {
    case 1: predict = *hist1 * 0x3C; break;
    case 2: predict = *hist1 * 0x73 - *hist2 * 0x34; break;
    case 3: predict = *hist1 * 0x62 - *hist2 * 0x37; break;
    default: break;
    }
    predict = (predict + 0x20) >> 6;
    if (predict < -0x200000) predict = -0x200000;
    if (predict > 0x1FFFFF) predict = 0x1FFFFF;
    cur = (((int32_t)(int16_t)(nibble << 12) >> (header & 0xF)) << 6) + predict;
    *hist2 = *hist1;
    *hist1 = cur;
    return (int16_t)gc_clamp16(cur >> 6);
}

/* The next ADPCM block from the ring into left/right; 0 when the ring
 * is empty (the stream has ended, or the card side has not caught up).
 * Runs in the AI interrupt: no card access here. */
static int gc_stream_block(struct gc_rt_stream* st) {
    const uint8_t* b;
    uint32_t i;
    if (st->fetched - st->consumed < GC_ADP_BLOCK) {
        if (st->ended) st->playing = 0;
        return 0;
    }
    b = (const uint8_t*)(uintptr_t)(st->buf + st->consumed % st->buf_bytes);
    for (i = 0; i < 28; ++i) {
        st->left[i] = gc_adp_sample(b[4 + i] & 0xF, b[0], &st->hist[0], &st->hist[1]);
        st->right[i] = gc_adp_sample(b[4 + i] >> 4, b[1], &st->hist[2], &st->hist[3]);
    }
    st->consumed += GC_ADP_BLOCK;  /* the card side may reuse the block from now on */
    st->sample = 0;
    return 1;
}

/* The next 48 kHz stereo sample; 0 when there is none. */
static int gc_stream_next(struct gc_rt_stream* st, int32_t* l, int32_t* r) {
    if (st->sample >= 28 && !gc_stream_block(st)) return 0;
    *l = st->left[st->sample];
    *r = st->right[st->sample];
    st->sample++;
    return 1;
}

/* AIInitDMA: the buffer the AI plays next is the game's finished mix;
 * the stream goes into it at the AI's stream volume. */
void gc_on_ai_dma(struct gc_rt_context* ctx, uint32_t start, uint32_t length) {
    struct gc_rt_stream* st = (struct gc_rt_stream*)(uintptr_t)ctx->stream;
    const uint32_t cr = gc_read32(GC_AI_CR);
    const uint32_t vr = gc_read32(GC_AI_VR);
    gc_guard(ctx);
    const int32_t vol_l = (int32_t)(vr & 0xFFu);
    const int32_t vol_r = (int32_t)((vr >> 8) & 0xFFu);
    const uint32_t addr = 0x80000000u | (start & 0x01FFFFE0u);
    int16_t* out = (int16_t*)(uintptr_t)addr;
    uint32_t frames = length / 4;
    uint32_t i;
    if ((++ctx->ai_dmas & 1023) == 0) gc_trace(ctx, GC_TRACE_AI);
    if (st == 0 || !st->playing || (cr & GC_AI_PSTAT) == 0 || length == 0) return;
    gc_invalidate(addr, length);
    for (i = 0; i < frames; ++i) {
        int32_t l;
        int32_t r;
        if (!gc_stream_next(st, &l, &r)) {
            if (st->playing) st->starved++;
            break;
        }
        if (cr & GC_AI_32K) {
            /* 48 to 32 kHz: of every three samples, the first, then the
             * other two averaged. */
            if (st->phase == 0) {
                st->phase = 1;
            } else {
                int32_t l2;
                int32_t r2;
                if (!gc_stream_next(st, &l2, &r2)) {
                    if (st->playing) st->starved++;
                    break;
                }
                l = (l + l2) >> 1;
                r = (r + r2) >> 1;
                st->phase = 0;
            }
        }
        if (ctx->flags & GC_FLAG_STREAM_ONLY) out[2 * i] = out[2 * i + 1] = 0;
        out[2 * i] = (int16_t)gc_clamp16(out[2 * i] + ((l * vol_l) >> 8));
        out[2 * i + 1] = (int16_t)gc_clamp16(out[2 * i + 1] + ((r * vol_r) >> 8));
    }
    gc_flush(addr, length);
    st->mixes++;
}

/* Back to RiftWii: the title the loader picked (its channel, else the
 * Homebrew Channel, else the Wii Menu), launched through /dev/es, which
 * IOS still runs in Wii mode. The screen goes black first (the VI's
 * display enable, DCR bit 0). Returns only when IOS refused. */
#define GC_ES_LAUNCHTITLE 8u
#define GC_VI_DCR 0xCC002002u
static void gc_leave(struct gc_rt_context* ctx) {
    const uint32_t buf = ctx->leave_buf;
    int32_t result;
    uint32_t msr;
    /* The memory card first: what the game saved is in MEM2 until a
     * heartbeat writes it back. A sector that will not write keeps the
     * game here, to try again. */
    if (ctx->card) {
        struct gc_card* card = (struct gc_card*)(uintptr_t)ctx->card;
        gc_card_flush(ctx, card, 1);
        if (card->dirty_count != 0) return;
    }
    gc_trace(ctx, GC_TRACE_LEAVE);
    msr = gc_irq_off();  /* the game stops here: IOS takes the console over */
    *(volatile uint16_t*)(uintptr_t)GC_VI_DCR &= (uint16_t)~1u;
    ctx->vec[0] = gc_physical(buf);
    ctx->vec[1] = 8;
    ctx->vec[2] = gc_physical(buf + 0x20);
    ctx->vec[3] = 0xD8;
    gc_flush(buf, 0x100);
    gc_flush((uint32_t)(uintptr_t)ctx->vec, sizeof(ctx->vec));
    ctx->busy = 1;
    ctx->ipc_no_timeout = 1;
    result = gc_ioctlv(ctx, ctx->leave_fd, GC_ES_LAUNCHTITLE, 2, 0);
    ctx->ipc_no_timeout = 0;
    /* Still here: IOS said no (the title went, or its ticket). The picture
     * and the game come back, and the trace says why. */
    ctx->busy = 0;
    ctx->last_error = result;
    *(volatile uint16_t*)(uintptr_t)GC_VI_DCR |= 1u;
    gc_irq_restore(msr);
    gc_trace(ctx, GC_TRACE_LEAVE);
}

/* HOME on a Wii Remote with an extension or on a Classic Controller (HOME
 * and - on a Wii Remote alone, whose HOME is the game's Y), or L, R, Z and
 * Start together on a GameCube controller (the pads as the game reads
 * them), held for leave_hold ticks; checked at every pad read. Not while
 * an SD request is in flight, nor within half a second of the game's last
 * memory card access (a save may be under way): the next read tries
 * again. */
#define GC_CARD_QUIET_TICKS 30375000u  /* half a second of the timebase */
#define GC_LEAVE_PAD 0x1070u  /* Start, L, R, Z */
static void gc_leave_check(struct gc_rt_context* ctx, const uint8_t* status) {
    uint32_t port, now;
    int held = 0;
    if (ctx->leave_buf == 0 || ctx->leave_fd == 0xFFFFFFFFu) return;
    if (ctx->bt && gc_bt_home_held((const struct gc_bt*)(uintptr_t)ctx->bt)) held = 1;
    for (port = 0; port < 4 && !held; ++port) {
        const uint8_t* pad = status + 12 * port;
        const uint32_t buttons = ((uint32_t)pad[0] << 8) | pad[1];
        if (pad[10] == 0 && (buttons & GC_LEAVE_PAD) == GC_LEAVE_PAD) held = 1;
    }
    if (!held) {
        ctx->leave_since = 0;
        return;
    }
    __asm__ volatile("mftb %0" : "=r"(now));
    if (ctx->card) {
        const struct gc_card* card = (const struct gc_card*)(uintptr_t)ctx->card;
        if (card->activity != ctx->leave_card_activity || ctx->leave_card_quiet == 0) {
            ctx->leave_card_activity = card->activity;
            ctx->leave_card_quiet = now | 1u;
        }
    }
    if (ctx->leave_since == 0) {
        ctx->leave_since = now | 1u;
        return;
    }
    if (now - ctx->leave_since < ctx->leave_hold || ctx->busy) return;
    if (ctx->card && now - ctx->leave_card_quiet < GC_CARD_QUIET_TICKS) return;
    gc_leave(ctx);
    ctx->leave_since = 0;
}

/* Test presses: every 240 reads, 10 reads of A, then B, then Start, on
 * port 1 (PADStatus: buttons at 0, err at 10; A 0x100, B 0x200, Start
 * 0x1000). */
/* GXRenderModeObj as VIConfigure reads it: viTVMode is the first word,
 * the TV format << 2 | 0 interlaced, 1 double-strike (240 lines), 2
 * progressive; xfbMode is the word at 0x14, 0 single field, 1 double
 * field (the XFB holds whole frames); field_rendering is the byte at 0x18. */
#define GC_RM_WORDS 15u
#define GC_RM_TV_MODE 0u
#define GC_RM_XFB_MODE 5u
#define GC_RM_FIELD_RENDERING 0x18u
#define GC_VI_NTSC_INTERLACED 0u
#define GC_VI_NTSC_PROGRESSIVE 2u
#define GC_VI_XFB_SINGLE 0u
#define GC_VI_XFB_DOUBLE 1u

/* VIConfigure(rm) with 480p on: an NTSC interlaced mode whose frames are
 * whole in the XFB goes on as its progressive twin, which shows the same
 * XFB a whole frame each retrace. A copy changes, never the game's own
 * mode, which it keeps using for its copies to the XFB. Modes drawn a
 * field at a time and double-strike (240-line) modes stay as they are:
 * their XFB holds half a picture. */
uint32_t gc_on_vi_configure(struct gc_rt_context* ctx, uint32_t mode) {
    const uint32_t* words = (const uint32_t*)(uintptr_t)mode;
    const uint8_t* bytes = (const uint8_t*)(uintptr_t)mode;
    uint32_t used = mode;
    uint32_t used_tv_mode;
    uint32_t i;
    ++ctx->vi_configures;
    if ((ctx->flags & GC_FLAG_PROGRESSIVE) && words[GC_RM_TV_MODE] == GC_VI_NTSC_INTERLACED &&
        words[GC_RM_XFB_MODE] == GC_VI_XFB_DOUBLE && bytes[GC_RM_FIELD_RENDERING] == 0) {
        for (i = 0; i < GC_RM_WORDS; ++i) ctx->vi_copy[i] = words[i];
        ctx->vi_copy[GC_RM_TV_MODE] = GC_VI_NTSC_PROGRESSIVE;
        ctx->vi_copy[GC_RM_XFB_MODE] = GC_VI_XFB_SINGLE;
        used = (uint32_t)(uintptr_t)ctx->vi_copy;
    }
    used_tv_mode = ((const uint32_t*)(uintptr_t)used)[GC_RM_TV_MODE];
    ctx->vi_mode = (words[GC_RM_TV_MODE] & 0xFFu) << 8 | (used_tv_mode & 0xFFu);
    return used;
}

/* A float's bits times 3/4, in integers: the blob does no floating point
 * (it runs on the game's FPU state, which it must leave as it found it),
 * so the widening is done on the bits. Zero, denormals, infinities and
 * NaNs come back as they are; a projection has none of them where it is
 * widened. */
static uint32_t gc_float_three_quarters(uint32_t bits) {
    const uint32_t sign = bits & 0x80000000u;
    uint32_t exponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa;
    if (exponent <= 1u || exponent == 0xFFu) return bits;
    /* The 24-bit mantissa with its hidden 1, times 3: 25 or 26 bits. The
     * value is that times 2^(exponent - 152); back to 24 bits, rounded. */
    mantissa = ((bits & 0x7FFFFFu) | 0x800000u) * 3u;
    if (mantissa >= 0x2000000u) {
        mantissa = (mantissa + 2u) >> 2;
    } else {
        mantissa = (mantissa + 1u) >> 1;
        exponent -= 1u;
    }
    if (mantissa == 0x1000000u) {  /* rounded up to the next power of two */
        mantissa >>= 1;
        exponent += 1u;
    }
    return sign | exponent << 23 | (mantissa & 0x7FFFFFu);
}

/* Notes the caller (its return address) of a perspective projection for
 * the trace: which of the game's passes are widened, with the shape of
 * the first matrix each one gave. */
static void gc_note_projection_caller(struct gc_rt_context* ctx, uint32_t caller, const uint32_t* matrix) {
    uint32_t i;
    for (i = 0; i < GC_PROJECTION_CALLERS; ++i) {
        uint32_t* slot = &ctx->projection_callers[i * 4];
        if (slot[0] == caller) {
            ++slot[1];
            return;
        }
        if (slot[0] == 0) {
            slot[0] = caller;
            slot[1] = 1;
            slot[2] = matrix[0];
            slot[3] = matrix[5];
            return;
        }
    }
}

/* GXSetProjection(mtx, type) with 16:9 on: a perspective projection
 * (type 0) is copied with its x scale, mtx[0][0], times 3/4: the same
 * view drawn into a picture a widescreen TV stretches by 4/3. An
 * orthographic one (type 1: menus, text, the 2D layers) is left alone.
 * GXSetProjection reads the copy at once, so one is enough, and the
 * game's own matrix is never changed (games set the same one every
 * frame). gc_hook_projection runs the original on what this returns. */
#define GC_GX_PERSPECTIVE 0u

uint32_t gc_on_projection(struct gc_rt_context* ctx, uint32_t mtx, uint32_t type, uint32_t caller) {
    const uint32_t* matrix = (const uint32_t*)(uintptr_t)mtx;
    uint32_t i;
    if (type != GC_GX_PERSPECTIVE) {
        ++ctx->orthographics;
        return mtx;
    }
    ++ctx->projections;
    gc_note_projection_caller(ctx, caller, matrix);
    for (i = 0; i < 16; ++i) ctx->projection[i] = matrix[i];
    if (ctx->flags & GC_FLAG_WIDEN) ctx->projection[0] = gc_float_three_quarters(matrix[0]);
    return (uint32_t)(uintptr_t)ctx->projection;
}

uint32_t gc_on_pad_read(struct gc_rt_context* ctx, uint32_t status, uint32_t result) {
    const uint32_t n = ctx->pad_reads++;
    gc_guard(ctx);  /* once a frame: IOS rewrites its counter there all the time */
    gc_stream_fetch(ctx);  /* the music ring's top-up, once a frame */
    if (ctx->bt && status != 0) {
        struct gc_bt* bt = (struct gc_bt*)(uintptr_t)ctx->bt;
        gc_bt_poll(ctx, bt);
        gc_bt_fill(bt, (uint8_t*)(uintptr_t)status);
    }
    if (n % 300 == 0) gc_trace(ctx, GC_TRACE_PAD);
    if ((ctx->flags & GC_FLAG_PRESS) && status != 0 && (n % 240) < 10) {
        uint8_t* p = (uint8_t*)(uintptr_t)status;
        const uint32_t which = (n / 240) % 3;
        const uint32_t bit = which == 0 ? 0x0100u : which == 1 ? 0x0200u : 0x1000u;
        const uint32_t now = ((uint32_t)p[0] << 8) | p[1];
        p[0] = (uint8_t)((now | bit) >> 8);
        p[1] = (uint8_t)(now | bit);
        p[10] = 0;
    }
    if ((ctx->flags & GC_FLAG_LEAVE) && status != 0 && n >= 1800) {
        uint8_t* p = (uint8_t*)(uintptr_t)status;
        p[0] |= (uint8_t)(GC_LEAVE_PAD >> 8);
        p[1] |= (uint8_t)GC_LEAVE_PAD;
        p[10] = 0;
    }
    if (status != 0) gc_leave_check(ctx, (const uint8_t*)(uintptr_t)status);
    if ((ctx->flags & GC_FLAG_CRASH) && n == 600) __asm__ volatile("trap");
    return result;
}

/* ARAM on a Wii. In Wii mode the ARAM DMA engine reports each transfer
 * done (its interrupt still comes) but moves nothing: there is no ARAM
 * behind it (measured on a Wii with test transfers; Dolphin emulates one).
 * So ARAM lives in MEM2 at physical 0x10000000 + the ARAM address, where
 * the DSP finds it too once its microcode is pointed there, and every
 * ARStartDMA is done here by the CPU before the game's own call runs, on
 * a scratch buffer, for the interrupt. Main memory comes as an address
 * the hardware masks. */
#define GC_ARAM_BASE 0x90000000u  /* MEM2, cached (Broadway's BAT4, left on) */
#define GC_ARAM_SIZE 0x01000000u
static void gc_aram_copy(uint32_t to_main, uint32_t main, uint32_t aram, uint32_t length) {
    const uint32_t m = 0x80000000u | (main & 0x01FFFFE0u);
    const uint32_t a = GC_ARAM_BASE | (aram & (GC_ARAM_SIZE - 1) & ~31u);
    const uint32_t* from = (const uint32_t*)(uintptr_t)(to_main ? a : m);
    uint32_t* to = (uint32_t*)(uintptr_t)(to_main ? m : a);
    uint32_t i;
    length &= ~31u;
    if ((aram & (GC_ARAM_SIZE - 1)) + length > GC_ARAM_SIZE) return;
    /* What the game flushed for the DMA is in memory; what it will read is
     * taken from memory after its DCInvalidate, so the copy goes there. */
    gc_invalidate((uint32_t)(uintptr_t)from, length);
    for (i = 0; i < length / 4; ++i) to[i] = from[i];
    gc_flush((uint32_t)(uintptr_t)to, length);
}

uint32_t gc_on_ar_dma(struct gc_rt_context* ctx, uint32_t type, uint32_t main, uint32_t aram, uint32_t length) {
    gc_aram_copy(type != 0, main, aram, length);
    ctx->ar_dmas++;
    ctx->last_ar[0] = type;
    ctx->last_ar[1] = main;
    ctx->last_ar[2] = aram;
    ctx->last_ar[3] = length;
    if (ctx->ar_dmas <= 16 || (ctx->ar_dmas & 255) == 0) gc_trace(ctx, GC_TRACE_AR);
    /* The game's own call still runs, for the interrupt it waits for, on
     * this buffer for 32 bytes, with its own ARAM address: an ARAM read on
     * a Wii writes zeros (there is no ARAM behind it) and would undo the
     * copy above; with the scratch's own address for ARAM instead, a Wii's
     * Luigi's Mansion stopped after 281 transfers, waiting.
     * Dolphin does perform the call, into MEM2 at the ARAM address, which
     * is the copy's own place: so the buffer holds the copy's first 32
     * bytes, and a transfer into ARAM writes them again unchanged. */
    {
        uint32_t* scratch = (uint32_t*)(uintptr_t)ctx->aram_scratch;
        const uint32_t* first = (const uint32_t*)(uintptr_t)(GC_ARAM_BASE | (aram & (GC_ARAM_SIZE - 1) & ~31u));
        uint32_t i;
        for (i = 0; i < 8; ++i) scratch[i] = first[i];
        gc_flush((uint32_t)(uintptr_t)scratch, 32);
    }
    return gc_physical((uint32_t)(uintptr_t)ctx->aram_scratch);
}

/* The loader's record just before the jump: the first one written through
 * the blob's own IPC, as the game will use it. */
void gc_on_trace_now(struct gc_rt_context* ctx, uint32_t where) { gc_trace(ctx, where); }

/* A crashed context's registers and the LRs saved up its stack (each
 * frame's back chain at 0, its caller's LR at 4 of the frame above),
 * while the frames stay in MEM1. */
static int gc_in_mem1(uint32_t a) { return a >= 0x80000000u && a < 0x81800000u && (a & 3u) == 0; }
static void gc_crash_stack(struct gc_rt_context* ctx, const uint32_t* gpr) {
    uint32_t frame = gpr[1];
    uint32_t i;
    ctx->crash_gpr[0] = gpr[1];
    ctx->crash_gpr[1] = gpr[3];
    ctx->crash_gpr[2] = gpr[4];
    ctx->crash_gpr[3] = gpr[5];
    ctx->crash_gpr[4] = gpr[31];
    for (i = 0; i < 4; ++i) {
        if (!gc_in_mem1(frame)) break;
        frame = *(const uint32_t*)(uintptr_t)frame;  /* the caller's frame */
        if (!gc_in_mem1(frame)) break;
        ctx->crash_chain[i] = *(const uint32_t*)(uintptr_t)(frame + 4);
    }
}

/* After a crash: the frame on screen white (through the uncached mirror,
 * so the VI shows it at once) and the disc slot's blue light flashing
 * (Hollywood GPIO, Broadway's bank, bit 5), since the game's own picture
 * may hide the frame. With the menu's message (crash_card), the picture
 * the VI shows now goes black with the message in the middle instead. */
#define GC_CRASH_WHITE 0xEB80EB80u
#define GC_YUYV_BLACK 0x10801080u
#define GC_GPIO_OUT 0xCD0000C0u
#define GC_GPIO_SLOT_LED 0x20u
#define GC_VI_VTR 0xCC002000u   /* bits 4-13: active lines per field */
#define GC_VI_TFBL 0xCC00201Cu  /* the top field's frame buffer */
#define GC_VI_BFBL 0xCC002024u  /* the bottom field's */
#define GC_VI_HSW 0xCC002048u   /* bits 0-7: a field line's stride, bits 8-14: its width, both in 32 bytes */
/* A frame buffer register's address: bits 0-23, shifted by 5 when bit 28
 * (the page offset) is set, as the SDK writes it. */
static uint32_t gc_vi_fb(uint32_t reg) {
    const uint32_t v = gc_read32(reg);
    return (v & 0x10000000u) ? (v & 0x00FFFFFFu) << 5 : (v & 0x00FFFFFFu);
}
/* The message's coverage at the frame's (x, y), 0 outside it. */
static uint32_t gc_card_at(const uint8_t* card, int32_t x, int32_t y) {
    uint32_t at;
    if (x < 0 || y < 0 || x >= (int32_t)GC_CRASH_CARD_W || y >= (int32_t)GC_CRASH_CARD_H) return 0;
    at = (uint32_t)y * GC_CRASH_CARD_W + (uint32_t)x;
    return ((card[at / 4u] >> (6u - 2u * (at % 4u))) & 3u) * 85u;
}
static uint32_t gc_luma(uint32_t coverage) { return 16u + coverage * 219u / 255u; }
static int gc_crash_message(const struct gc_rt_context* ctx) {
    const uint8_t* card = (const uint8_t*)(uintptr_t)ctx->crash_card;
    const uint32_t top = gc_vi_fb(GC_VI_TFBL), bottom = gc_vi_fb(GC_VI_BFBL);
    const uint16_t hsw = *(volatile uint16_t*)(uintptr_t)GC_VI_HSW;
    const uint32_t stride = (hsw & 0xFFu) * 32u, words = ((hsw >> 8) & 0x7Fu) * 32u / 4u;
    const uint32_t lines = (*(volatile uint16_t*)(uintptr_t)GC_VI_VTR >> 4) & 0x3FFu;
    /* A bottom field only when its register holds a frame buffer too. */
    const uint32_t fields = bottom != top && bottom != 0 && bottom < 0x01800000u ? 2u : 1u;
    const int32_t width = (int32_t)(words * 2u), height = (int32_t)(lines * fields);
    uint32_t field, k, w;
    if (card == 0 || top == 0 || top >= 0x01800000u || stride == 0 || words == 0 || lines == 0) return 0;
    for (field = 0; field < fields; ++field) {
        const uint32_t base = field == 0 ? top : bottom;
        for (k = 0; k < lines; ++k) {
            volatile uint32_t* row = (volatile uint32_t*)(uintptr_t)((base + k * stride) | 0xC0000000u);
            const int32_t y = (int32_t)(k * fields + field) - (height - (int32_t)GC_CRASH_CARD_H) / 2;
            if (base + k * stride + words * 4u > 0x01800000u) break;
            for (w = 0; w < words; ++w) {
                /* Two pixels a word: Y0 U Y1 V, grey on black. */
                const int32_t x = (int32_t)(w * 2u) - (width - (int32_t)GC_CRASH_CARD_W) / 2;
                const uint32_t y0 = gc_luma(gc_card_at(card, x, y)), y1 = gc_luma(gc_card_at(card, x + 1, y));
                row[w] = (y0 << 24) | 0x800000u | (y1 << 8) | 0x80u;  /* coverage 0: GC_YUYV_BLACK */
            }
        }
    }
    return 1;
}
static void gc_crash_screen(const struct gc_rt_context* ctx) {
    volatile uint32_t* p;
    uint32_t i;
    if (gc_crash_message(ctx)) return;
    if (ctx->xfb == 0) return;
    p = (volatile uint32_t*)(uintptr_t)(gc_physical(ctx->xfb) | 0xC0000000u);
    for (i = 0; i < ctx->xfb_bytes / 4; ++i) p[i] = GC_CRASH_WHITE;
}
/* A pressed on a Wii Remote or a GameCube controller in port 1 (its SI
 * buffer, which the SI goes on filling with interrupts off: A is bit 24 of
 * the first word; in Wii mode the SI sits at 0xCD006400). */
#define GC_SI_C0INBUFH 0xCD006404u
/* A down now: port 1's SI word with no error (ERRSTAT, bit 31), a Wii
 * Remote's A or a Classic Controller's a (its sixth byte's 0x10, active
 * low). */
static int gc_crash_a_down(struct gc_rt_context* ctx) {
    uint32_t i;
    const uint32_t si = gc_read32(GC_SI_C0INBUFH);
    if (!(si & 0x80000000u) && (si & 0x01000000u)) return 1;
    if (ctx->bt) {
        struct gc_bt* bt = (struct gc_bt*)(uintptr_t)ctx->bt;
        gc_bt_poll(ctx, bt);
        for (i = 0; i < GC_BT_REMOTES; ++i) {
            const struct gc_bt_remote* r = &bt->remotes[i];
            if (!r->used || !r->ready) continue;
            if (r->core[1] & 0x08u) return 1;
            if (r->ext_kind == GC_BT_EXT_CLASSIC && r->ext_step == 4 && !(r->ext[5] & 0x10u)) return 1;
        }
    }
    return 0;
}
/* A pressed: down now and up at the last look, so the A the game crashed
 * under (often its main button) or one still held after IOS refused does
 * not count; the player sees the message first. */
static int gc_crash_a(struct gc_rt_context* ctx) {
    const int down = gc_crash_a_down(ctx);
    const int pressed = down && !ctx->crash_a_was_down;
    ctx->crash_a_was_down = (uint32_t)down;
    return pressed;
}
/* The light flashes; A goes back to RiftWii when there is a way back. */
/* About a frame (16 ms of the timebase): how often the controllers are
 * looked at, the pace gc_bt's start-up counts in. */
#define GC_CRASH_POLL_TICKS 972000u
static void gc_crash_wait(struct gc_rt_context* ctx) {
    uint32_t start, now, polled;
    __asm__ volatile("mftb %0" : "=r"(start));
    polled = start;
    ctx->crash_a_was_down = 1;  /* the first look only learns where A is */
    for (;;) {
        __asm__ volatile("mftb %0" : "=r"(now));
        if (now - start >= 10000000u) {
            start = now;
            gc_write32(GC_GPIO_OUT, gc_read32(GC_GPIO_OUT) ^ GC_GPIO_SLOT_LED);
        }
        if (now - polled < GC_CRASH_POLL_TICKS) continue;
        polled = now;
        if (gc_crash_a(ctx) && ctx->leave_buf != 0 && ctx->leave_fd != 0xFFFFFFFFu) gc_leave(ctx);
    }
}

/* The game crashed: the GameCube OS's last stop for an exception nothing
 * handles, which would halt with a blank screen on a retail console. The
 * crash goes to the trace (the exception, SRR0/LR/SRR1, DSISR, DAR, some
 * registers and the callers up the stack), then the screen turns white. */
void gc_on_fatal(struct gc_rt_context* ctx, uint32_t exception, uint32_t os_context, uint32_t dsisr, uint32_t dar) {
    ctx->exception = exception;
    ctx->dsisr = dsisr;
    ctx->dar = dar;
    if (os_context != 0) {
        ctx->beat_pc = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_SRR0);
        ctx->beat_lr = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_LR);
        ctx->beat_msr = *(const uint32_t*)(uintptr_t)(os_context + GC_OSCONTEXT_SRR1);
        gc_crash_stack(ctx, (const uint32_t*)(uintptr_t)os_context);
        if (gc_in_mem1(ctx->beat_pc))
            ctx->crash_word = gc_read32(gc_physical(ctx->beat_pc) | 0xC0000000u);
    }
    ctx->busy = 0;  /* a crash inside a read leaves it set; nothing else runs now */
    gc_trace(ctx, GC_TRACE_FATAL);
    gc_crash_screen(ctx);
    gc_crash_wait(ctx);
}
