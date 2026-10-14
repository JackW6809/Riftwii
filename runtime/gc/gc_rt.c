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
    ctx->vec[5] = sizeof(ctx->response);
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
    return (int32_t)rq[1];
}

/* One record of the trace (gc_rt.h), with interrupts held off so it
 * cannot meet a read on the card; skipped while a read is going on. */
static void gc_trace(struct gc_rt_context* ctx, uint32_t where) {
    uint32_t* w = (uint32_t*)(uintptr_t)ctx->trace_buf;
    const struct gc_rt_stream* st = (const struct gc_rt_stream*)(uintptr_t)ctx->stream;
    uint32_t* q = ctx->request;
    uint32_t msr, tbu, tbl, i, sector;
    if (ctx->trace_sector == 0 || ctx->trace_count == 0 || w == 0) return;
    msr = gc_irq_off();
    if (ctx->busy) {
        gc_irq_restore(msr);
        return;
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
    sector = ctx->trace_sector + (ctx->trace_seq - 1) % ctx->trace_count;
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
    ctx->vec[5] = sizeof(ctx->response);
    gc_flush((uint32_t)(uintptr_t)q, 36);
    gc_flush((uint32_t)(uintptr_t)ctx->vec, sizeof(ctx->vec));
    gc_flush(ctx->trace_buf, GC_SECTOR);
    if (gc_ioctlv(ctx, ctx->sdio_fd, GC_SDIO_SENDCMD, 2, 1) >= 0 && ctx->card_rca != 0) {
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

uint32_t gc_on_read(struct gc_rt_context* ctx, uint32_t addr, uint32_t length, uint32_t offset, uint32_t cb) {
    ctx->commands++;
    ctx->reads++;
    ctx->read_bytes += length;
    ctx->last_offset = offset;
    gc_complete(ctx, cb, gc_read_disc(ctx, (uint8_t*)(uintptr_t)addr, length, offset) ? GC_DI_TC : GC_DI_DE);
    if (ctx->reads <= 32 || (ctx->reads & 63) == 0) gc_trace(ctx, GC_TRACE_READ);
    return 1;
}

uint32_t gc_on_disk_id(struct gc_rt_context* ctx, uint32_t id, uint32_t cb) {
    ctx->commands++;
    gc_complete(ctx, cb, gc_read_disc(ctx, (uint8_t*)(uintptr_t)id, 0x20, 0) ? GC_DI_TC : GC_DI_DE);
    return 1;
}

/* DVDDriveInfo: revision, device code, release date, then padding. */
uint32_t gc_on_inquiry(struct gc_rt_context* ctx, uint32_t info, uint32_t cb) {
    uint32_t* words = (uint32_t*)(uintptr_t)info;
    uint32_t i;
    ctx->commands++;
    for (i = 0; i < 8; ++i) words[i] = 0;
    words[0] = 0x00020201u;
    words[1] = 0x20020402u;
    gc_flush(info, 0x20);
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

uint32_t gc_on_done(struct gc_rt_context* ctx, uint32_t cb) {
    ctx->commands++;
    gc_set_imm(ctx, 0);  /* RequestError: no error */
    gc_complete(ctx, cb, GC_DI_TC);
    return 1;
}

typedef void (*gc_callback_fn)(uint32_t type);
void gc_on_alarm(struct gc_rt_context* ctx) {
    const uint32_t cb = ctx->pending_cb;
    ctx->alarms++;
    ctx->pending_cb = 0;
    if (cb != 0) ((gc_callback_fn)(uintptr_t)cb)(ctx->pending_type);
}

/* ---- Disc audio streaming ------------------------------------------ */

#define GC_AI_CR 0xCC006C00u
#define GC_AI_VR 0xCC006C04u
#define GC_AI_PSTAT 0x01u   /* the stream plays */
#define GC_AI_32K 0x40u     /* the DMA (the game's own audio) runs at 32 kHz */
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
    ctx->commands++;
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
    ctx->commands++;
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
    const uint32_t cr = gc_read32(GC_AI_CR);
    const uint32_t vr = gc_read32(GC_AI_VR);
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

void gc_on_ar_dma(struct gc_rt_context* ctx, uint32_t type, uint32_t main, uint32_t aram, uint32_t length) {
    ctx->ar_dmas++;
    ctx->last_ar[0] = type;
    ctx->last_ar[1] = main;
    ctx->last_ar[2] = aram;
    ctx->last_ar[3] = length;
    if (ctx->ar_dmas <= 16 || (ctx->ar_dmas & 255) == 0) gc_trace(ctx, GC_TRACE_AR);
}
