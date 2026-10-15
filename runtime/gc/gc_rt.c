/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube disc blob's C part (gc_rt.h). Same rules as the other
 * blobs: no globals, no literals that need relocations, no library
 * calls; all state is in the context the trampolines hand over. */
#include "gc_rt.h"

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

static uint32_t gc_read32(uint32_t address) { return *(volatile uint32_t*)(uintptr_t)address; }
static void gc_write32(uint32_t address, uint32_t value) { *(volatile uint32_t*)(uintptr_t)address = value; }

static uint32_t gc_physical(uint32_t address) { return address & 0x3FFFFFFFu; }

static void gc_flush(uint32_t address, uint32_t length) {
    uint32_t a = address & ~31u;
    const uint32_t end = address + length;
    for (; a < end; a += 32) __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static void gc_invalidate(uint32_t address, uint32_t length) {
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
static void gc_guard(struct gc_rt_context* ctx) {
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

/* One ioctlv to IOS and its reply, by polling: the request is the only
 * one in flight (the menu's IPC ended with libogc). Returns IOS's result,
 * or -1000 when IOS does not answer. */
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
    gc_write32(GC_IPC_PPCMSG, phys);
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_X1);
    for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y2) == 0; ++polls)
        if (polls == GC_IPC_TIMEOUT) return -1000;
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y2);
    for (;;) {
        for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y1) == 0; ++polls)
            if (polls == GC_IPC_TIMEOUT) return -1000;
        {
            const uint32_t reply = gc_read32(GC_IPC_ARMMSG);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y1);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_X2);
            if (reply == phys) break;  /* a reply for anything else is not ours */
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

static uint32_t gc_irq_off(void) {
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr & ~0x8000u) : "memory");
    return msr;
}
static void gc_irq_restore(uint32_t msr) { __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory"); }

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
    gc_write32(GC_IPC_PPCMSG, phys);
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_X1);
    for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y2) == 0; ++polls)
        if (polls == GC_IPC_TIMEOUT) return -1000;
    gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y2);
    for (;;) {
        for (polls = 0; (gc_read32(GC_IPC_PPCCTRL) & GC_IPC_Y1) == 0; ++polls)
            if (polls == GC_IPC_TIMEOUT) return -1000;
        {
            const uint32_t reply = gc_read32(GC_IPC_ARMMSG);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_Y1);
            gc_write32(GC_IPC_PPCCTRL, GC_IPC_X2);
            if (reply == phys) break;
        }
    }
    gc_invalidate((uint32_t)(uintptr_t)rq, sizeof(ctx->ipc));
    gc_invalidate(out, out_len);
    gc_guard(ctx);
    return (int32_t)rq[1];
}

/* The whole frame in one colour, through the uncached mirror so the VI
 * shows it at once. */
static void gc_paint(const struct gc_rt_context* ctx, uint32_t colour) {
    volatile uint32_t* p;
    uint32_t i;
    if (ctx->xfb == 0) return;
    p = (volatile uint32_t*)(uintptr_t)(gc_physical(ctx->xfb) | 0xC0000000u);
    for (i = 0; i < ctx->xfb_bytes / 4; ++i) p[i] = colour;
}

/* The disc slot's blue light (Hollywood GPIO, Broadway's bank, bit 5):
 * a second signal beside the screen, since a game that set up its own
 * video hides the colours. */
#define GC_GPIO_OUT 0xCD0000C0u
#define GC_GPIO_SLOT_LED 0x20u
static void gc_led(int on) {
    const uint32_t out = gc_read32(GC_GPIO_OUT);
    gc_write32(GC_GPIO_OUT, on ? (out | GC_GPIO_SLOT_LED) : (out & ~GC_GPIO_SLOT_LED));
}
static void gc_led_toggle(void) { gc_write32(GC_GPIO_OUT, gc_read32(GC_GPIO_OUT) ^ GC_GPIO_SLOT_LED); }
/* After a crash: the light flashing fast, a few times a second. */
static void gc_led_toggle_slowly(void) {
    uint32_t start, now;
    __asm__ volatile("mftb %0" : "=r"(start));
    do {
        __asm__ volatile("mftb %0" : "=r"(now));
    } while (now - start < 10000000u);
    gc_led_toggle();
}

/* A filled box on the frame, in YUYV words (two pixels) across and lines
 * down; the frame is 640 pixels wide. */
#define GC_XFB_WORDS_PER_LINE 320u
static void gc_box(const struct gc_rt_context* ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour) {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)(gc_physical(ctx->xfb) | 0xC0000000u);
    const uint32_t lines = ctx->xfb_bytes / (GC_XFB_WORDS_PER_LINE * 4);
    uint32_t row, col;
    for (row = y; row < y + h && row < lines; ++row)
        for (col = x; col < x + w && col < GC_XFB_WORDS_PER_LINE; ++col) p[row * GC_XFB_WORDS_PER_LINE + col] = colour;
}

/* `value` as eight hex digits, seven-segment style, on line `row` of the
 * frame: the numbers a test on a Wii needs when nothing else can show
 * them. Segments a-g are bits 0-6; the codes are in immediates because
 * the blob keeps no constant data. */
#define GC_WHITE 0xEB80EB80u
#define GC_BLACK 0x10801080u
static uint32_t gc_segments(uint32_t digit) {
    /* Four digits a word, 0 first (a switch becomes a table in .rodata). */
    uint32_t four;
    if (digit < 4)
        four = 0x3F065B4Fu;
    else if (digit < 8)
        four = 0x666D7D07u;
    else if (digit < 12)
        four = 0x7F6F777Cu;
    else
        four = 0x395E7971u;
    return (four >> (24 - 8 * (digit & 3u))) & 0x7Fu;
}
static void gc_show_hex(const struct gc_rt_context* ctx, uint32_t row, uint32_t value) {
    const uint32_t top = 30 + row * 75;
    uint32_t n;
    if (ctx->xfb == 0) return;
    gc_box(ctx, 30, top - 8, 8 * 24 + 8, 56 + 16, GC_BLACK);
    for (n = 0; n < 8; ++n) {
        const uint32_t seg = gc_segments((value >> (28 - 4 * n)) & 0xFu);
        const uint32_t x = 38 + n * 24;
        if (seg & 0x01) gc_box(ctx, x, top, 16, 6, GC_WHITE);
        if (seg & 0x02) gc_box(ctx, x + 13, top, 3, 28, GC_WHITE);
        if (seg & 0x04) gc_box(ctx, x + 13, top + 28, 3, 28, GC_WHITE);
        if (seg & 0x08) gc_box(ctx, x, top + 50, 16, 6, GC_WHITE);
        if (seg & 0x10) gc_box(ctx, x, top + 28, 3, 28, GC_WHITE);
        if (seg & 0x20) gc_box(ctx, x, top, 3, 28, GC_WHITE);
        if (seg & 0x40) gc_box(ctx, x, top + 25, 16, 6, GC_WHITE);
    }
}

/* One record of the trace (gc_rt.h), with interrupts held off so it
 * cannot meet a read on the card; skipped while a read is going on. */
/* Record `seq`'s sector: the first records stay (how the game started),
 * the last GC_TRACE_RING go round. */
static uint32_t gc_trace_slot(const struct gc_rt_context* ctx, uint32_t seq) {
    const uint32_t kept = ctx->trace_count > 2 * GC_TRACE_RING ? ctx->trace_count - GC_TRACE_RING : ctx->trace_count / 2;
    const uint32_t ring = ctx->trace_count - kept;
    if (seq <= kept) return ctx->trace_sector + seq - 1;
    return ctx->trace_sector + kept + (seq - kept - 1) % ring;
}

#define GC_TRACE_SKIPPED (-3000)  /* no trace, or the IPC buffers were in use */
static int32_t gc_trace(struct gc_rt_context* ctx, uint32_t where) {
    int32_t result;
    uint32_t* w = (uint32_t*)(uintptr_t)ctx->trace_buf;
    const struct gc_rt_stream* st = (const struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t* q = ctx->request;
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
    }
    w[GC_TR_AI_DMAS] = ctx->ai_dmas;
    w[GC_TR_AR_DMAS] = ctx->ar_dmas;
    for (i = 0; i < 4; ++i) w[GC_TR_LAST_AR_TYPE + i] = ctx->last_ar[i];
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
    for (i = 0; i < 4; ++i) w[GC_TR_PROBE + i] = ctx->probe[i];
    if (ctx->watch_addr != 0) {
        w[GC_TR_WATCH_ADDR] = ctx->watch_addr;
        w[GC_TR_WATCH_NOW] = gc_read32(gc_physical(ctx->watch_addr) | 0xC0000000u);
        w[GC_TR_WATCH_WAS] = ctx->watch_was;
    }
    /* Interrupts: the PI's cause and mask, and Hollywood's lines to the
     * CPU (Wii-only; a GameCube game cannot clear them). */
    w[GC_TR_PI_CAUSE] = gc_read32(0xCC003000u);
    w[GC_TR_PI_MASK] = gc_read32(0xCC003004u);
    w[GC_TR_HW_IRQ_FLAG] = gc_read32(0xCD000030u);
    w[GC_TR_HW_IRQ_MASK] = gc_read32(0xCD000034u);
    sector = gc_trace_slot(ctx, ctx->trace_seq);
    /* CMD25, one block; then CMD13 until the card has written it. */
    q[0] = 0x19u;
    q[1] = GC_SD_CMDTYPE_AC;
    q[2] = GC_SD_RESPONSE_R1;
    q[3] = ctx->sdhc ? sector : sector * GC_SECTOR;
    q[4] = 1;
    q[5] = GC_SECTOR;
    q[6] = ctx->trace_buf;
    q[7] = 1;
    q[8] = 0;
    ctx->vec[0] = gc_physical((uint32_t)(uintptr_t)q);
    ctx->vec[1] = 36;
    ctx->vec[2] = gc_physical(ctx->trace_buf);
    ctx->vec[3] = GC_SECTOR;
    ctx->vec[4] = gc_physical((uint32_t)(uintptr_t)ctx->response);
    ctx->vec[5] = GC_SD_RESPONSE_BYTES;
    gc_flush((uint32_t)(uintptr_t)q, 36);
    gc_flush((uint32_t)(uintptr_t)ctx->vec, sizeof(ctx->vec));
    gc_flush(ctx->trace_buf, GC_SECTOR);
    result = gc_ioctlv(ctx, ctx->sdio_fd, GC_SDIO_SENDCMD, 2, 1);
    if (result >= 0) gc_led(1);
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
static void gc_complete(struct gc_rt_context* ctx, uint32_t cb, uint32_t type) {
    gc_set_alarm_fn set_alarm = (gc_set_alarm_fn)(uintptr_t)ctx->set_alarm;
    ctx->pending_cb = cb;
    ctx->pending_type = type;
    set_alarm(ctx->alarm, ctx->delay_ticks, ctx->alarm_entry);
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

uint32_t gc_on_read(struct gc_rt_context* ctx, uint32_t addr, uint32_t length, uint32_t offset, uint32_t cb) {
    gc_command_seen(ctx, 0);
    ctx->reads++;
    ctx->read_bytes += length;
    ctx->last_offset = offset;
    gc_complete(ctx, cb, gc_read_disc(ctx, (uint8_t*)(uintptr_t)addr, length, offset) ? GC_DI_TC : GC_DI_DE);
    if (ctx->reads == 1) gc_paint(ctx, GC_COLOUR_MAGENTA);
    if ((ctx->reads & 7) == 0) gc_led_toggle();
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
        gc_trace(ctx, GC_TRACE_BEAT);
        gc_beat_arm(ctx);
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
#define GC_AI_32K 0x40u     /* the DMA (the game's own audio) runs at 32 kHz */
#define GC_AI_AIINT 0x08u   /* write 1 to clear: kept out of our writes */
#define GC_AI_SCRESET 0x20u /* write 1 to reset the sample counter: likewise */
#define GC_ADP_BLOCK 32u    /* 28 stereo samples */

static void gc_stream_reset(struct gc_rt_stream* st) {
    st->hist[0] = st->hist[1] = st->hist[2] = st->hist[3] = 0;
    st->sample = 28;
    st->phase = 0;
    st->buf_valid = 0;
    st->buf_at = 0;
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
        const uint32_t played = st->pos - (st->buf_valid - st->buf_at);
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

/* The next ADPCM block into left/right; 0 when the stream has ended (or
 * the card is busy with the game's own read). */
static int gc_stream_block(struct gc_rt_context* ctx, struct gc_rt_stream* st) {
    const uint8_t* b;
    uint32_t i;
    if (st->buf_at + GC_ADP_BLOCK > st->buf_valid) {
        const uint32_t end = st->start + st->length;
        uint32_t n;
        if (st->pos >= end) {
            if (st->next_length == 0) {
                st->playing = 0;
                return 0;
            }
            st->start = st->next_start;
            st->length = st->next_length;
            st->pos = st->start;
        }
        n = st->start + st->length - st->pos;
        if (n > st->buf_bytes) n = st->buf_bytes;
        n &= ~(GC_ADP_BLOCK - 1);
        if (n == 0 || !gc_read_disc(ctx, (uint8_t*)(uintptr_t)st->buf, n, st->pos)) return 0;
        st->pos += n;
        st->buf_valid = n;
        st->buf_at = 0;
        st->refills++;
    }
    b = (const uint8_t*)(uintptr_t)(st->buf + st->buf_at);
    st->buf_at += GC_ADP_BLOCK;
    for (i = 0; i < 28; ++i) {
        st->left[i] = gc_adp_sample(b[4 + i] & 0xF, b[0], &st->hist[0], &st->hist[1]);
        st->right[i] = gc_adp_sample(b[4 + i] >> 4, b[1], &st->hist[2], &st->hist[3]);
    }
    st->sample = 0;
    return 1;
}

/* The next 48 kHz stereo sample; 0 when there is none. */
static int gc_stream_next(struct gc_rt_context* ctx, struct gc_rt_stream* st, int32_t* l, int32_t* r) {
    if (st->sample >= 28 && !gc_stream_block(ctx, st)) return 0;
    *l = st->left[st->sample];
    *r = st->right[st->sample];
    st->sample++;
    return 1;
}

/* AIInitDMA: the buffer the AI plays next is the game's finished mix;
 * the stream goes into it at the AI's stream volume. */
void gc_on_ai_dma(struct gc_rt_context* ctx, uint32_t start, uint32_t length) {
    struct gc_rt_stream* st = (struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t cr = gc_read32(GC_AI_CR);
    const uint32_t vr = gc_read32(GC_AI_VR);
    gc_guard(ctx);
    /* A GameCube's audio DMA always runs at 32 kHz; a Wii's runs at 48 kHz
     * unless this bit is set (Wii only), which played the game's sound
     * 1.5 times too fast on a Wii. */
    if (ctx->ai_dmas == 0) ctx->probe[3] = cr;  /* the game's own setting, for the trace */
    if ((cr & GC_AI_32K) == 0) {
        gc_write32(GC_AI_CR, (cr & ~(GC_AI_AIINT | GC_AI_SCRESET)) | GC_AI_32K);
        cr |= GC_AI_32K;
    }
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
        if (!gc_stream_next(ctx, st, &l, &r)) break;
        if (cr & GC_AI_32K) {
            /* 48 to 32 kHz: of every three samples, the first, then the
             * other two averaged. */
            if (st->phase == 0) {
                st->phase = 1;
            } else {
                int32_t l2;
                int32_t r2;
                if (!gc_stream_next(ctx, st, &l2, &r2)) break;
                l = (l + l2) >> 1;
                r = (r + r2) >> 1;
                st->phase = 0;
            }
        }
        out[2 * i] = (int16_t)gc_clamp16(out[2 * i] + ((l * vol_l) >> 8));
        out[2 * i + 1] = (int16_t)gc_clamp16(out[2 * i + 1] + ((r * vol_r) >> 8));
    }
    gc_flush(addr, length);
    st->mixes++;
}

/* Test presses: every 240 reads, 10 reads of A, then B, then Start, on
 * port 1 (PADStatus: buttons at 0, err at 10; A 0x100, B 0x200, Start
 * 0x1000). */
uint32_t gc_on_pad_read(struct gc_rt_context* ctx, uint32_t status, uint32_t result) {
    const uint32_t n = ctx->pad_reads++;
    gc_guard(ctx);  /* once a frame: IOS rewrites its counter there all the time */
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
    return result;
}

/* ARAM on a Wii. In Wii mode the ARAM DMA engine reports each transfer
 * done (its interrupt still comes) but moves nothing: there is no ARAM
 * behind it (measured on a Wii with gc_aram_probe; Dolphin emulates one).
 * So ARAM lives in MEM2 at physical 0x10000000 + the ARAM address, where
 * the DSP finds it too once its microcode is pointed there, and every
 * ARStartDMA is done here by the CPU before the game's own call runs, on
 * a scratch buffer, for the interrupt. Main memory comes as an address the hardware masks. */
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
    /* The game's own call still runs, for the interrupt it waits for, but
     * on this buffer: an ARAM read on a Wii writes zeros (there is no ARAM
     * behind it) and would undo the copy above. */
    return gc_physical((uint32_t)(uintptr_t)ctx->aram_scratch);
}

/* Trial: where ARAM is on a Wii. Three 32-byte ARAM DMAs through the
 * DSP's registers, before the game owns them:
 *   probe[0]: MRAM -> ARAM 0x10f00000, then the first word at MEM2
 *             0x10f00000 (the pattern if ARAM addresses are physical);
 *   probe[1]: MRAM -> ARAM at the bounce buffer's own physical address
 *             + 0x2000, then the first word there (the pattern if ARAM
 *             below 0x10000000 is MEM1);
 *   probe[2]: ARAM 0x10f00000 -> MRAM, the first word read back;
 *   probe[3]: polls left after each DMA (a byte each, 0: it timed out). */
#define GC_DSP_CSR 0xCC00500Au
#define GC_DSP_AR_MODE 0xCC005012u
#define GC_DSP_CSR_DMA_BUSY 0x0200u
static void gc_write16(uint32_t address, uint32_t value) { *(volatile uint16_t*)(uintptr_t)address = (uint16_t)value; }
static uint32_t gc_read16(uint32_t address) { return *(volatile uint16_t*)(uintptr_t)address; }
static uint32_t gc_aram_dma(uint32_t to_mram, uint32_t mram, uint32_t aram, uint32_t length) {
    uint32_t polls;
    gc_write16(0xCC005020u, mram >> 16);
    gc_write16(0xCC005022u, mram & 0xFFFFu);
    gc_write16(0xCC005024u, aram >> 16);
    gc_write16(0xCC005026u, aram & 0xFFFFu);
    gc_write16(0xCC005028u, to_mram ? 0x8000u : 0);
    gc_write16(0xCC00502Au, length);
    for (polls = 255; polls != 0; --polls) {
        uint32_t spin;
        if ((gc_read16(GC_DSP_CSR) & GC_DSP_CSR_DMA_BUSY) == 0) break;
        for (spin = 0; spin < 2000; ++spin) __asm__ volatile("nop");
    }
    /* Its interrupt acknowledged (ARINT), the DSP's and the AI's left alone. */
    gc_write16(GC_DSP_CSR, (gc_read16(GC_DSP_CSR) & ~0x88u) | 0x20u);
    return polls;
}
static void gc_aram_probe(struct gc_rt_context* ctx) {
    uint32_t* src = (uint32_t*)(uintptr_t)(ctx->bounce + 0x1000);
    const uint32_t mem1_target = ctx->bounce + 0x2000;
    const uint32_t back = ctx->bounce + 0x3000;
    uint32_t i, p0, p1, p2;
    gc_write16(GC_DSP_AR_MODE, (gc_read16(GC_DSP_AR_MODE) & ~0x3Fu) | 0x23u);  /* 16 MB, as ARInit sets it */
    for (i = 0; i < 8; ++i) src[i] = 0xA5A50000u + i;
    gc_flush((uint32_t)(uintptr_t)src, 32);
    gc_write32(0xD0F00000u, 0);
    gc_write32(gc_physical(mem1_target) | 0xC0000000u, 0);
    gc_write32(gc_physical(back) | 0xC0000000u, 0);
    p0 = gc_aram_dma(0, gc_physical((uint32_t)(uintptr_t)src), 0x10F00000u, 32);
    ctx->probe[0] = gc_read32(0xD0F00000u);
    p1 = gc_aram_dma(0, gc_physical((uint32_t)(uintptr_t)src), gc_physical(mem1_target), 32);
    ctx->probe[1] = gc_read32(gc_physical(mem1_target) | 0xC0000000u);
    p2 = gc_aram_dma(1, gc_physical(back), 0x10F00000u, 32);
    ctx->probe[2] = gc_read32(gc_physical(back) | 0xC0000000u);
    ctx->probe[3] = (p0 << 16) | (p1 << 8) | p2;
    gc_trace(ctx, GC_TRACE_PROBE);
}

/* Before the jump, every step tried whatever the one before gave, and
 * its result shown as a number on the frame (a line each, top down):
 *   1. IOS's answer to the trace record's write (CMD25)
 *   2. IOS's answer to GETSTATUS, a plain ioctl, and the status word
 *   3. IOS's answer to reading the record back (CMD18), then 1 if it matches
 *   4. the image's first bytes read through the runs: 1 if they are the ID
 * -1000 (FFFFFC18) is "IOS never answered". The background is the old
 * colour: cyan when all passed, red when the write failed, orange when it
 * does not read back, grey when the image does not. */
#define GC_SDIO_GETSTATUS 0x0Bu
void gc_on_trace_now(struct gc_rt_context* ctx, uint32_t where) {
    const uint8_t* id = (const uint8_t*)(uintptr_t)0x80000000u;
    uint8_t* back = (uint8_t*)(uintptr_t)ctx->bounce;
    const uint32_t* w = (const uint32_t*)(uintptr_t)ctx->trace_buf;
    const uint32_t seq = ctx->trace_seq + 1;
    int32_t written, status, readback;
    uint32_t matches, image_ok, i;
    uint32_t colour = GC_COLOUR_CYAN;
    written = gc_trace(ctx, where);
    ctx->response[0] = 0;
    status = gc_ioctl(ctx, ctx->sdio_fd, GC_SDIO_GETSTATUS, 0, 0, (uint32_t)(uintptr_t)ctx->response, 4);
    for (i = 0; i < GC_SECTOR; ++i) back[i] = 0;
    readback = gc_sd_read(ctx, gc_trace_slot(ctx, seq), 1, ctx->bounce) ? 0 : ctx->last_error;
    matches = ((const uint32_t*)back)[0] == w[0] && ((const uint32_t*)back)[1] == seq;
    for (i = 0; i < 0x20; ++i) back[i] = 0;
    image_ok = (uint32_t)gc_read_disc(ctx, back, 0x20, 0);
    for (i = 0; i < 6; ++i)
        if (back[i] != id[i]) image_ok = 0;
    if (written < 0)
        colour = GC_COLOUR_RED;
    else if (!matches)
        colour = GC_COLOUR_ORANGE;
    else if (!image_ok)
        colour = GC_COLOUR_GREY;
    gc_paint(ctx, colour);
    gc_show_hex(ctx, 0, (uint32_t)written);
    gc_show_hex(ctx, 1, (uint32_t)status);
    gc_show_hex(ctx, 2, ctx->response[0]);
    gc_show_hex(ctx, 3, (uint32_t)readback);
    gc_show_hex(ctx, 4, matches);
    gc_show_hex(ctx, 5, image_ok);
    gc_aram_probe(ctx);
}

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

/* The game crashed: the GameCube OS's last stop for an exception nothing
 * handles, which would halt with a blank screen on a retail console. The
 * crash goes to the trace (all of it) and onto the frame, a row each:
 *   1. the exception (2 DSI, 3 ISI, 5 alignment, 6 program...) in the top
 *      byte, SRR1's reason bits, DSISR's top half
 *   2. SRR0, where it happened
 *   3. and 4. the two callers up the stack
 *   5. r31 and 6. r5 (for memset: where it started and how much). */
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
    __asm__ volatile("mtspr 1013, %0" : : "r"(0) : "memory");  /* the loader's write watch off (DABR) */
    ctx->busy = 0;  /* a crash inside a read leaves it set; nothing else runs now */
    gc_trace(ctx, GC_TRACE_FATAL);
    gc_paint(ctx, GC_COLOUR_WHITE);
    gc_show_hex(ctx, 0, (exception << 24) | (ctx->beat_msr & 0x00FF0000u) | (dsisr >> 16));
    gc_show_hex(ctx, 1, ctx->beat_pc);
    gc_show_hex(ctx, 2, ctx->crash_chain[0]);
    gc_show_hex(ctx, 3, ctx->crash_chain[1]);
    gc_show_hex(ctx, 4, ctx->crash_gpr[4]);
    gc_show_hex(ctx, 5, ctx->crash_gpr[3]);
    for (;;) gc_led_toggle_slowly();
}
