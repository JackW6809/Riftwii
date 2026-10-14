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
    .long gc_hook_cb_r3 - _gc_blob_start, gc_hook_cb_r4 - _gc_blob_start
    .long gc_hook_cb_r5 - _gc_blob_start, gc_hook_cb_r6 - _gc_blob_start
    .long gc_alarm - _gc_blob_start

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
    GC_DONE cb_r4, 4
    GC_DONE cb_r5, 5
    GC_DONE cb_r6, 6

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

/* The OSAlarm handler (alarm, context): runs the pending callback. */
    .globl gc_alarm
gc_alarm:
    stwu 1, -0x20(1)
    mflr 0
    stw 0, 0x24(1)
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
    .fill 95, 4, 0
