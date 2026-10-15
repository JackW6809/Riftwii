/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube disc blob's header and trampolines (gc_rt.h). Each hook
 * replaces a DVDLow* function at its first word: the game calls it with
 * its own arguments and gets 1 (the command was taken) back. */
    .section .text.gc_entry, "ax", @progbits
    .globl _gc_blob_start
_gc_blob_start:
    .long 0x52574744, 1, _gc_blob_end - _gc_blob_start, gc_context_data - _gc_blob_start
    .long gc_hook_read - _gc_blob_start, gc_hook_seek - _gc_blob_start
    .long gc_hook_disk_id - _gc_blob_start, gc_hook_inquiry - _gc_blob_start
    .long gc_hook_cb_r3 - _gc_blob_start, gc_hook_audio_status - _gc_blob_start
    .long gc_hook_cb_r5 - _gc_blob_start, gc_hook_audio_stream - _gc_blob_start
    .long gc_alarm - _gc_blob_start
    .long gc_hook_ai_dma - _gc_blob_start, gc_replay_ai_dma - _gc_blob_start, gc_continue_ai_dma - _gc_blob_start
    .long gc_hook_pad_read - _gc_blob_start, gc_replay_pad_read - _gc_blob_start, gc_continue_pad_read - _gc_blob_start
    .long gc_hook_ar_dma - _gc_blob_start, gc_replay_ar_dma - _gc_blob_start, gc_continue_ar_dma - _gc_blob_start
    .long gc_trace_now - _gc_blob_start
    .long gc_hook_fatal - _gc_blob_start

/* Calls `fn` with the context in r3 and the game's r3..r6 moved up one
 * register (r4..r7). */
    .macro GC_HOOK name, fn
    .globl gc_hook_\name
gc_hook_\name:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    mr 7, 6
    mr 6, 5
    mr 5, 4
    mr 4, 3
    bl 1f
1:  mflr 3
    addi 3, 3, gc_context_data - 1b
    bl \fn
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    blr
    .endm

    GC_HOOK read, gc_on_read
    GC_HOOK disk_id, gc_on_disk_id
    GC_HOOK inquiry, gc_on_inquiry
    GC_HOOK audio_status, gc_on_audio_status
    GC_HOOK audio_stream, gc_on_audio_stream
    GC_HOOK fatal, gc_on_fatal

/* AIInitDMA(start, length): gc_on_ai_dma(ctx, start, length) mixes the
 * disc stream into the buffer, then the original runs from the replay
 * slot with the game's arguments. Frame: 8 start, 12 length. */
    .globl gc_hook_ai_dma
gc_hook_ai_dma:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    stw 3, 0x08(1)
    stw 4, 0x0C(1)
    mr 5, 4
    mr 4, 3
    bl 4f
4:  mflr 3
    addi 3, 3, gc_context_data - 4b
    bl gc_on_ai_dma
    lwz 3, 0x08(1)
    lwz 4, 0x0C(1)
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    b gc_replay_ai_dma

/* u32 PADRead(PADStatus status[4]): the original through its replay
 * slot, then gc_on_pad_read(ctx, status, its result). */
    .globl gc_hook_pad_read
gc_hook_pad_read:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    stw 3, 0x08(1)
    bl gc_replay_pad_read
    mr 5, 3
    lwz 4, 0x08(1)
    bl 5f
5:  mflr 3
    addi 3, 3, gc_context_data - 5b
    bl gc_on_pad_read
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    blr

    .balign 16
    .globl gc_replay_pad_read
gc_replay_pad_read:
    nop
    nop
    nop
    nop
    .globl gc_continue_pad_read
gc_continue_pad_read:
    lis 12, 0
    ori 12, 12, 0
    mtctr 12
    bctr

/* ARStartDMA(type, main, aram, length): gc_on_ar_dma counts it, then
 * the original runs from its replay slot with the game's arguments.
 * Frame: 8 r3, 12 r4, 16 r5, 20 r6. */
    .globl gc_hook_ar_dma
gc_hook_ar_dma:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    stw 3, 0x08(1)
    stw 4, 0x0C(1)
    stw 5, 0x10(1)
    stw 6, 0x14(1)
    mr 7, 6
    mr 6, 5
    mr 5, 4
    mr 4, 3
    bl 6f
6:  mflr 3
    addi 3, 3, gc_context_data - 6b
    bl gc_on_ar_dma
    lwz 3, 0x08(1)
    lwz 4, 0x0C(1)
    lwz 5, 0x10(1)
    lwz 6, 0x14(1)
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    b gc_replay_ar_dma

    .balign 16
    .globl gc_replay_ar_dma
gc_replay_ar_dma:
    nop
    nop
    nop
    nop
    .globl gc_continue_ar_dma
gc_continue_ar_dma:
    lis 12, 0
    ori 12, 12, 0
    mtctr 12
    bctr

/* Loader-filled: AIInitDMA's first word (and nops), then the jump to its
 * second word through r12. */
    .balign 16
    .globl gc_replay_ai_dma
gc_replay_ai_dma:
    nop
    nop
    nop
    nop
    .globl gc_continue_ai_dma
gc_continue_ai_dma:
    lis 12, 0
    ori 12, 12, 0
    mtctr 12
    bctr

/* Commands with nothing to do but their callback, in r3..r6: moved to
 * r4 for gc_on_done(ctx, cb). */
    .macro GC_DONE name, reg
    .globl gc_hook_\name
gc_hook_\name:
    mr 4, \reg
    b gc_done_common
    .endm

    GC_DONE seek, 4
    GC_DONE cb_r3, 3
    GC_DONE cb_r5, 5

gc_done_common:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    bl 2f
2:  mflr 3
    addi 3, 3, gc_context_data - 2b
    bl gc_on_done
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    blr

/* trace_now(where): one trace record, for the loader. */
    .globl gc_trace_now
gc_trace_now:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    mr 4, 3
    bl 7f
7:  mflr 3
    addi 3, 3, gc_context_data - 7b
    bl gc_on_trace_now
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    blr

/* The OSAlarm handler (alarm, context): runs the pending callback, or
 * takes the heartbeat's sample of the interrupted context. */
    .globl gc_alarm
gc_alarm:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
    mr 5, 4
    mr 4, 3
    bl 3f
3:  mflr 3
    addi 3, 3, gc_context_data - 3b
    bl gc_on_alarm
    lwz 0, 0x24(1)
    mtlr 0
    addi 1, 1, 0x20
    blr

    .balign 32
    .globl gc_context_data
gc_context_data:
    .long 0x52574743
    .fill 135, 4, 0
