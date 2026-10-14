/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GameCube games in Wii mode, without the disc drive: the game's
 * DVDLow* functions branch here and are served from the SD card.
 *
 * A GameCube game drives the disc through the DI registers and waits
 * for the DI interrupt; in Wii mode those belong to IOS. So the SDK's
 * low-level commands are replaced: a read is done at once through IOS's
 * /dev/sdio/slot0 (IPC driven by polling, since the game has no IPC
 * code and its interrupt table knows nothing of IOS), and the command's
 * callback runs from one of the game's own OSAlarms a moment later, as
 * the DI interrupt would have run it.
 *
 * Layout (gc_entry.S): the header below, the trampolines, the C code,
 * then the context. Position independent (linked at two bases and
 * compared); it sits in MEM1 just below the FST, under the game's arena.
 * Every buffer IOS touches is in the context, 32-byte aligned. */
#ifndef RIFTWII_GC_RT_H
#define RIFTWII_GC_RT_H

#include <stdint.h>

#define GC_RT_MAGIC 0x52574744u          /* "RWGD" */
#define GC_RT_VERSION 1u                 /* also in gc_entry.S's header */
#define GC_RT_CONTEXT_MAGIC 0x52574743u  /* "RWGC" */

/* The DI interrupt types a DVDLow callback takes. */
#define GC_DI_TC 1u     /* transfer complete */
#define GC_DI_DE 2u     /* device error */

/* gc_rt_run.kind */
#define GC_RUN_SD 0u    /* `source` is a 512-byte sector of the card, `skip` bytes into it */
#define GC_RUN_ZERO 1u
#define GC_RUN_MEM 2u   /* `source` is an address (bytes the loader placed in memory) */

/* One stretch of the game's view of the disc. Sorted by `disc`, no gaps
 * the game reads (a read past the last run fails). */
struct gc_rt_run {
    uint32_t disc;
    uint32_t length;
    uint32_t source;
    uint32_t skip;
    uint32_t kind;
};

/* Big-endian words at the start of the blob: offsets from its start. */
struct gc_rt_header {
    uint32_t magic;
    uint32_t version;
    uint32_t size;            /* code, then the context */
    uint32_t context_offset;
    uint32_t hook_read;       /* DVDLowRead(addr, length, offset, cb) */
    uint32_t hook_seek;       /* DVDLowSeek(offset, cb) */
    uint32_t hook_disk_id;    /* DVDLowReadDiskID(id, cb) */
    uint32_t hook_inquiry;    /* DVDLowInquiry(info, cb) */
    uint32_t hook_cb_r3;      /* DVDLowStopMotor(cb), DVDLowRequestError(cb) */
    uint32_t hook_audio_status;  /* DVDLowRequestAudioStatus(sub, cb) */
    uint32_t hook_cb_r5;      /* DVDLowAudioBufferConfig(on, size, cb) */
    uint32_t hook_audio_stream;  /* DVDLowAudioStream(sub, length, offset, cb) */
    uint32_t alarm_entry;     /* the OSAlarm handler that runs the callback */
    uint32_t hook_ai_dma;     /* AIInitDMA(start, length): the stream mixed in, then the original */
    uint32_t replay_ai_dma;   /* 16 bytes: AIInitDMA's displaced first word, then padding */
    uint32_t continue_ai_dma; /* 16 bytes: the jump to its second word */
    uint32_t hook_pad_read;   /* PADRead(status): the original, then the test presses (GC_FLAG_PRESS) */
    uint32_t replay_pad_read;
    uint32_t continue_pad_read;
};

/* gc_rt_context.flags */
#define GC_FLAG_PRESS 1u  /* tests without a controller: port 1 presses A, B and Start in turn */

/* Disc audio streaming (the drive's own ADPCM, 48 kHz stereo): what the
 * drive would have played into the AI is decoded here and mixed into the
 * game's audio as it goes out. In MEM1 beside the blob (loader-placed). */
struct gc_rt_stream {
    uint32_t playing;         /* a stream is set (AudioStream with a length) */
    uint32_t start;           /* the stream, disc bytes */
    uint32_t length;
    uint32_t pos;             /* the next ADPCM block to decode */
    uint32_t next_start;      /* played after this one ends (the drive's queue); 0 length: stop */
    uint32_t next_length;
    uint32_t buf;             /* ADPCM read ahead from the disc, 32-byte aligned */
    uint32_t buf_bytes;
    uint32_t buf_valid;
    uint32_t buf_at;
    int32_t hist[4];          /* left hist1, hist2, right hist1, hist2 */
    int16_t left[28];
    int16_t right[28];
    uint32_t sample;          /* the next of left/right to use, 28: decode another block */
    uint32_t phase;           /* 48 to 32 kHz: two of every three samples */
    uint32_t mixes;
    uint32_t refills;
    uint32_t pad[2];
};

/* The OSAlarm the callbacks run from: the SDK's is 0x28 bytes. */
#define GC_ALARM_BYTES 0x40u
#define GC_IPC_TIMEOUT 50000000u  /* polls of the IPC registers before giving up */

struct gc_rt_context {
    uint32_t magic;
    /* Loader-filled. */
    uint32_t sdio_fd;         /* /dev/sdio/slot0, the card selected */
    uint32_t sdhc;            /* 1: CMD18 takes a sector, 0: a byte offset */
    uint32_t runs;            /* struct gc_rt_run[run_count] */
    uint32_t run_count;
    uint32_t set_alarm;       /* the game's OSSetAlarm */
    uint32_t alarm_entry;     /* this blob's alarm handler */
    uint32_t bounce;          /* 32-byte aligned, bounce_bytes (a multiple of 512) */
    uint32_t bounce_bytes;
    uint32_t delay_ticks;     /* how soon the callback runs */
    /* State. */
    uint32_t pending_cb;      /* the callback the alarm runs, 0: none */
    uint32_t pending_type;
    /* Counters, for the log and a memory dump. */
    uint32_t commands;
    uint32_t reads;
    uint32_t read_bytes;
    uint32_t sd_requests;
    uint32_t failures;
    int32_t last_error;
    uint32_t last_offset;
    uint32_t di_regs;         /* where the game's DI register reads were sent (its 0xCC006000) */
    uint32_t stream;          /* struct gc_rt_stream */
    uint32_t flags;           /* GC_FLAG_* */
    uint32_t pad_reads;
    uint32_t pad[9];
    uint8_t alarm[GC_ALARM_BYTES] __attribute__((aligned(32)));
    uint32_t ipc[16] __attribute__((aligned(32)));      /* the IPC request */
    uint32_t vec[8] __attribute__((aligned(32)));       /* three vectors */
    uint32_t request[16] __attribute__((aligned(32)));  /* the SENDCMD block (36 bytes) */
    uint32_t response[8] __attribute__((aligned(32)));
};

#define GC_RT_CONTEXT_BYTES 0x180u
typedef char gc_rt_context_size[(sizeof(struct gc_rt_context) == GC_RT_CONTEXT_BYTES) ? 1 : -1];

#ifdef RT_TARGET_PPC
uint32_t gc_on_read(struct gc_rt_context* ctx, uint32_t addr, uint32_t length, uint32_t offset, uint32_t cb);
uint32_t gc_on_disk_id(struct gc_rt_context* ctx, uint32_t id, uint32_t cb);
uint32_t gc_on_inquiry(struct gc_rt_context* ctx, uint32_t info, uint32_t cb);
uint32_t gc_on_done(struct gc_rt_context* ctx, uint32_t cb);
uint32_t gc_on_audio_status(struct gc_rt_context* ctx, uint32_t sub, uint32_t cb);
uint32_t gc_on_audio_stream(struct gc_rt_context* ctx, uint32_t sub, uint32_t length, uint32_t offset, uint32_t cb);
uint32_t gc_on_pad_read(struct gc_rt_context* ctx, uint32_t status, uint32_t result);
void gc_on_ai_dma(struct gc_rt_context* ctx, uint32_t start, uint32_t length);
void gc_on_alarm(struct gc_rt_context* ctx);
#endif

#endif
