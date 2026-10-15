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
    uint32_t hook_ar_dma;     /* ARStartDMA(type, main, aram, length): counted, then the original */
    uint32_t replay_ar_dma;
    uint32_t continue_ar_dma;
    uint32_t trace_now;       /* trace(where): a record from the loader, just before the jump */
    uint32_t hook_fatal;      /* __OSUnhandledException(exception, context, dsisr, dar): shown, never returns */
    uint32_t dsi_entry;       /* an exception before the game's OS has its handlers (the loader's vector stubs) */
};

/* gc_on_fatal's exception number for one caught by the loader's own
 * vectors, before the game's OS replaced them: 0x10 plus the vector
 * (0x12 DSI, 0x13 ISI, 0x16 program...). */
#define GC_EXCEPTION_EARLY 0x10u

/* The trace: a ring of 512-byte records in sd:/riftwii/gc/trace.bin
 * (loader-made, its sectors known), written now and then while the game
 * runs, so a run on a Wii that ends on a black screen says how far it
 * got. Big-endian words. */
#define GC_TRACE_MAGIC 0x52575452u  /* "RWTR" */
enum gc_trace_word {
    GC_TR_MAGIC, GC_TR_SEQ, GC_TR_WHERE, GC_TR_TBU, GC_TR_TBL, GC_TR_COMMANDS, GC_TR_READS, GC_TR_READ_BYTES,
    GC_TR_SD_REQUESTS, GC_TR_FAILURES, GC_TR_LAST_ERROR, GC_TR_LAST_OFFSET, GC_TR_PAD_READS, GC_TR_STREAM_PLAYING,
    GC_TR_STREAM_MIXES, GC_TR_STREAM_REFILLS, GC_TR_AI_DMAS, GC_TR_AR_DMAS, GC_TR_LAST_AR_TYPE, GC_TR_LAST_AR_MAIN,
    GC_TR_LAST_AR_ARAM, GC_TR_LAST_AR_LENGTH, GC_TR_PENDING_CB, GC_TR_ALARMS, GC_TR_BEATS, GC_TR_BEAT_PC,
    GC_TR_BEAT_LR, GC_TR_BEAT_MSR, GC_TR_EXCEPTION, GC_TR_DSISR, GC_TR_DAR,
    GC_TR_PI_CAUSE, GC_TR_PI_MASK, GC_TR_HW_IRQ_FLAG, GC_TR_HW_IRQ_MASK,
    GC_TR_CRASH_R1, GC_TR_CRASH_R3, GC_TR_CRASH_R4, GC_TR_CRASH_R5, GC_TR_CRASH_R31, GC_TR_CRASH_CHAIN, /* 4 words */
    GC_TR_CRASH_WORD = GC_TR_CRASH_CHAIN + 4, /* the word in memory (uncached) at SRR0 */
    GC_TR_WATCH_ADDR, GC_TR_WATCH_NOW, GC_TR_WATCH_WAS, /* a word watched for being overwritten */
    GC_TR_GUARD_HITS, GC_TR_GUARD_FIRST_REQUEST, GC_TR_GUARD_FIRST_OFFSET, GC_TR_GUARD_FIRST_VALUE,
    GC_TR_WORDS
};
/* where: what wrote the record */
#define GC_TRACE_READ 1u
#define GC_TRACE_PAD 2u
#define GC_TRACE_AI 3u
#define GC_TRACE_AR 4u
#define GC_TRACE_JUMP 5u
#define GC_TRACE_BEAT 6u
#define GC_TRACE_COMMAND 8u /* a DVDLow command that is not a read */
#define GC_TRACE_FATAL 7u   /* the game crashed: beat_pc/lr/msr are the crash's SRR0, LR, SRR1 */
/* The last records of the trace go round; the others stay. */
#define GC_TRACE_RING 16u

/* Whole-screen colours (YUYV pairs) a run on a Wii shows until the game
 * sets up its own video: how far it got when nothing else can say. */
#define GC_COLOUR_BLUE 0x1DFF1D6Bu     /* RiftWii shut down */
#define GC_COLOUR_GREEN 0x952B9515u    /* the game's code in place */
#define GC_COLOUR_YELLOW 0xE100E194u   /* the first trace record about to go */
#define GC_COLOUR_CYAN 0xB3ABB300u     /* it went: the card and the polled IPC work */
#define GC_COLOUR_RED 0x4C544CFFu      /* it did not: IOS did not answer */
#define GC_COLOUR_MAGENTA 0x69D469EAu  /* the game's first disc read through the blob */
#define GC_COLOUR_ORANGE 0xAD1EADC9u   /* the record was taken but does not read back */
#define GC_COLOUR_GREY 0x80808080u     /* it reads back, but the game image's first bytes do not */
#define GC_COLOUR_WHITE 0xEB80EB80u    /* the game crashed (numbers on it: gc_on_fatal) */

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
    uint32_t trace_sector;    /* the trace's first sector, 0: no trace */
    uint32_t trace_count;     /* its records */
    uint32_t trace_buf;       /* 512 bytes, 32-byte aligned */
    uint32_t trace_seq;
    uint32_t card_rca;        /* for CMD13 after a write */
    uint32_t ai_dmas;
    uint32_t ar_dmas;
    uint32_t alarms;
    uint32_t last_ar[4];
    uint32_t busy;            /* the IPC buffers are in use (a read or a trace write) */
    uint32_t xfb;             /* the frame on screen at the jump (YUYV), painted as steps pass; 0: none */
    uint32_t xfb_bytes;
    /* The heartbeat: an OSAlarm of its own every half second once the
     * game reads, noting where the game was when it went off (the
     * decrementer interrupt's saved context), for a run that hangs. */
    uint32_t beat_ticks;      /* loader-filled, 0: no heartbeat */
    uint32_t beats;
    uint32_t beat_pc;
    uint32_t beat_lr;
    uint32_t beat_msr;
    uint32_t exception;       /* set by gc_on_fatal */
    uint32_t dsisr;
    uint32_t dar;
    uint32_t crash_gpr[5];    /* r1, r3, r4, r5, r31 */
    uint32_t crash_chain[4];  /* the saved LRs up the stack: who called whom */
    uint32_t crash_word;
    uint32_t watch_addr;      /* loader-filled, 0: none: a word of the game's that a Wii finds overwritten */
    uint32_t watch_was;       /* its value at the jump */
    /* The guard: IOS keeps live globals at 0x80003100-0x800031ff on a Wii
     * and writes them while it works (a counter at 0x800031a0 at least),
     * but a GameCube game has code there. The loader's copy of the game's
     * bytes is compared after every IOS request and written back. */
    uint32_t guard_addr;      /* loader-filled, 0: none */
    uint32_t guard_hits;      /* words found changed and put back */
    uint32_t guard_first_request; /* sd_requests when the first was found */
    uint32_t guard_first_offset;
    uint32_t guard_first_value;
    uint32_t guard_copy[64];
    uint8_t alarm[GC_ALARM_BYTES] __attribute__((aligned(32)));
    uint8_t beat_alarm[GC_ALARM_BYTES] __attribute__((aligned(32)));
    uint32_t ipc[16] __attribute__((aligned(32)));      /* the IPC request */
    uint32_t vec[8] __attribute__((aligned(32)));       /* three vectors */
    uint32_t request[16] __attribute__((aligned(32)));  /* the SENDCMD block (36 bytes) */
    uint32_t response[8] __attribute__((aligned(32)));
};

#define GC_RT_CONTEXT_BYTES 0x340u
typedef char gc_rt_context_size[(sizeof(struct gc_rt_context) == GC_RT_CONTEXT_BYTES) ? 1 : -1];

#ifdef RT_TARGET_PPC
uint32_t gc_on_read(struct gc_rt_context* ctx, uint32_t addr, uint32_t length, uint32_t offset, uint32_t cb);
uint32_t gc_on_disk_id(struct gc_rt_context* ctx, uint32_t id, uint32_t cb);
uint32_t gc_on_inquiry(struct gc_rt_context* ctx, uint32_t info, uint32_t cb);
uint32_t gc_on_done(struct gc_rt_context* ctx, uint32_t cb);
uint32_t gc_on_audio_status(struct gc_rt_context* ctx, uint32_t sub, uint32_t cb);
uint32_t gc_on_audio_stream(struct gc_rt_context* ctx, uint32_t sub, uint32_t length, uint32_t offset, uint32_t cb);
void gc_on_trace_now(struct gc_rt_context* ctx, uint32_t where);
void gc_on_ar_dma(struct gc_rt_context* ctx, uint32_t type, uint32_t main, uint32_t aram, uint32_t length);
uint32_t gc_on_pad_read(struct gc_rt_context* ctx, uint32_t status, uint32_t result);
void gc_on_ai_dma(struct gc_rt_context* ctx, uint32_t start, uint32_t length);
void gc_on_alarm(struct gc_rt_context* ctx, uint32_t alarm, uint32_t os_context);
void gc_on_fatal(struct gc_rt_context* ctx, uint32_t exception, uint32_t os_context, uint32_t dsisr, uint32_t dar);
#endif

#endif
