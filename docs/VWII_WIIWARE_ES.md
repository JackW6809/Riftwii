# WiiWare on a Wii U: "ES would not run it as its own title"

Analysis for issue #128, written 2026-10-18 against `main` at 06e4322.
No code was changed. Where a fact comes from the code it has a file and
line; where it comes from public documentation it says which; anything
marked **unverified** is a reading that a tester's run has to confirm.

## The report

On a Wii U in Wii mode (vWii: IOS58 rev 6432, d2x cIOS in slots 248-251
on bases 38, 56, 57 and 58), WiiWare games (2 Fast 4 Gnomz, And Yet It
Moves, Bit.Trip Runner) do not start: RiftWii says "ES would not run it
as its own title on any IOS tried". One tester's log elsewhere has
"on IOS56, ES would not run RiftWii as WRTW01 (-4100)" and "d2x could
not mount the SD card (FAT module -6, mount -1)".

## How a channel game is started

1. `read_channel_boot` (`wii/storage/nandchannels.cpp:385`) reads, under
   the running IOS:
   - the TMD: from ES (`ES_GetStoredTMD`) for the Wii's own NAND, or
     `<nand>/title/<hi>/<lo>/content/title.tmd` from an emulated NAND on
     the SD card (`:401-416`);
   - the program content (`:432-456`);
   - the ticket: the **whole file** `/ticket/<hi>/<lo>.tik`, up to
     0x1000 bytes, from the NAND or the emulated NAND (`:457-464`). It is
     the full signed ticket, not a ticket view;
   - the certificates: `/sys/cert.sys`, the emulated NAND's copy first
     (`:465-468`).
2. It builds the IOS list `files.ios_order` (`:421-430`):
   - not in Dolphin: `image_cios_order(as_image, 0)`, keeping only slots
     that have a ticket and pass `d2x_slot_allowed` (d2x v11 or newer);
   - then the IOS the TMD asks for (`required_ios`), which on a vWii is
     Nintendo's IOS56/57/58 for these games.
3. `image_cios_order` (`wii/storage/usbcatalog.cpp:1621`) gives, in
   order:
   1. on a Wii U, with the GameCube adapter in use (**On**, or
      **Automatic** with the adapter seen), the base-58 d2x slot
      (`wii_u_adapter_slot`);
   2. `gx_pick_cios` (`src/patching/gxpatches.cpp:531`): the d2x slot
      whose base is the IOS the game asks for, among `d2x_slots()`;
   3. then 249, 250 and 251, whichever are not in the list yet.

   Slot 248 is tried only when `gx_pick_cios` picks it, that is when it
   holds the base the game asks for and no lower slot has that base.
4. `boot_channel` (`wii/launch/boot.cpp:3079`) tries each IOS in turn
   (`:3116-3161`):
   - it reloads the IOS (`reload_ios`); a slot that will not start is
     logged and skipped;
   - for a game of an **emulated NAND** it first turns on d2x's NAND
     emulation (`enable_d2x_saves`, `wii/storage/d2xsave.cpp:282`). When
     that fails, the IOS is **skipped without calling ES_Identify**
     (`boot.cpp:3134-3143`);
   - it calls `ES_Identify(certs, tmd, ticket)` (`:3146-3152`). The
     first success ends the loop; each failure logs "on IOS<n>, ES would
     not run RiftWii as <ID> (<code>)" (`:3159`).
5. With no success: "ES would not run it as its own title on any IOS
   tried" (`:3170`). `StartChannel` (`wii/main.cpp:84`) then:
   - for a game of the Wii's own NAND, lets the system start it with
     `WII_LaunchTitle` (libogc: `ES_LaunchTitle` with the title's ticket
     view), without RiftWii's extras (`:103-109`);
   - for a game of an **emulated NAND**, has nothing to fall back on:
     it goes back to RiftWii with "The game did not start: ES would not
     run it as its own title on any IOS tried" (`:95-101`). That is the
     message in the report, so the reported games are most likely in an
     emulated NAND (`sd:/nands/01`), or the system start failed as well.

## What ES_Identify needs

From wiibrew's `/dev/es` and Ticket/TMD pages (public documentation) and
libogc's `ES_Identify` wrapper:

- **The certificate chain** (`/sys/cert.sys`), **the TMD** and **one
  ticket**. ES checks their signatures, then makes the calling process
  run as that title. Its title ID and group ID then decide which
  `/title/.../data` the game may open.
- **The caller is checked.** On Nintendo's IOSes only the system menu's
  process may call it. RiftWii's own comment says so
  (`wii/storage/nandchannels.hpp:73`), and Dolphin's independent ES does
  the same (`boot.cpp:3162-3164`). cIOSes such as d2x patch that check
  out: this is why the d2x slots are tried first. **Unverified** for the
  vWii builds of d2x v11 on every base.
- **The ticket's console ID** (offset 0x1D8 of a ticket, per wiibrew's
  Ticket page). A shop ticket is personalised to the console that bought
  it. ES compares it with the console's own ID: wiibrew lists −1020 as
  "device ID mismatch". A WiiWare game moved from a Wii to a Wii U with
  the Wii U Transfer Tool should have been given a ticket for the new
  console; **unverified**.
- **The common key index** (offset 0x1F1): 0 for the Wii's common key,
  1 for the Korean key, 2 for the vWii key. vWii-only titles use index 2.
  WiiWare bought in the Wii Shop on a Wii U is, by everything public,
  the same content with index 0. ES_Identify decrypts the title key with
  whichever index the ticket names, and a vWii IOS holds all three, so
  the index alone should not make it fail. **Unverified.**
- **One ticket, not a file of several.** A `.tik` file can hold more
  than one ticket of 0x2A4 bytes each, for example after a re-download or
  a transfer. RiftWii passes the whole file and its size
  (`nandchannels.cpp:457-464`). Whether ES then rejects the call or reads
  only the first ticket is **unverified**, and it is the first thing to
  check in a tester's run (log the ticket size).
- **The TMD's IOS** does not matter to ES_Identify itself. It matters to
  the game, which RiftWii tells it got that IOS (`boot.cpp`, the 0x3140
  write after loading).

**−4100.** No published ES error table found for this analysis lists
−4100 (wiibrew's `/dev/es` page and its revisions list −1009 to −1035
and IOSC's −2011 and nearby). It came back on IOS56, which on a vWii is
Nintendo's IOS56. That is the case the code expects to fail, since
Nintendo's ES keeps ES_Identify to the system menu. So −4100 is most
likely that refusal (a permission error), not a fault in the files.
**Unverified**: a run that logs the code on each d2x slot as well would
show whether the d2x slots fail differently.

**FAT module −6, mount −1** (`d2xsave.cpp:300-326`). `IOS_Open("fat")`
returned −6, IOS's "no such file or device" (libogc's `IPC_ENOENT`). The
running IOS has no `fat` device: either it is not d2x (Nintendo's
required IOS, the last one in the list, never has one), or the vWii d2x
build loaded in that slot has no FAT module. With an emulated NAND this
failure makes `boot_channel` skip that IOS without calling ES_Identify
(`boot.cpp:3137-3141`). If it happens on every d2x slot, the emulated-NAND
game cannot start at all. The failing line names the folder but not the
IOS; the line before it in `boot.log` ("Saves: d2x NAND emulation from
...") is logged under the IOS just reloaded.

## What differs on a vWii

- Nintendo's IOS56/57/58 are vWii builds (IOS58 rev 6432 in the report).
  As on a Wii, they are not expected to allow ES_Identify to RiftWii.
- The d2x vWii installer puts base 58 in 251 (the comment at
  `usbcatalog.cpp:1615`). The tester has four d2x slots, 248-251, on
  bases 38, 56, 57 and 58.
- With the GameCube adapter seen, the base-58 slot goes first
  (`usbcatalog.cpp:1626-1640`). It is the only d2x that reaches a Wii U's
  front USB ports.
- The SD card: d2x reads it through its own SDHC and FAT modules. On a
  vWii the same card can work in libfat and still fail in d2x (the
  "mount" result). **Unverified** which slots' modules mount it.

## Most likely causes, in order

1. **The games are in an emulated NAND, and d2x's NAND emulation fails
   on every d2x slot.** The −6 means no `fat` device on that IOS. Every
   d2x slot is then skipped before ES_Identify, the last attempt is
   Nintendo's IOS56, and there is no system fallback for an emulated
   NAND. That explains the on-screen message and the FAT line together.
2. **The ticket passed is not one ticket**, or not this console's. d2x's
   ES_Identify would then refuse it with a signature or device-ID error
   (−1020, −2011) on every slot, and Nintendo's IOS56 with −4100.
3. **The vWii d2x build does not lift the ES_Identify caller check.**
   Every slot would then answer as IOS56 does, with the same code.

## Proposals

None of these were made: each needs a vWii run to decide.

1. **Log what decides it.** In `boot_channel`, after each reload: the IOS
   number and revision (`IOS_GetVersion`, `IOS_GetRevision`), whether it
   is d2x and its base (`slot_info`), and the code `enable_d2x_saves`
   failed with, naming that IOS. Once, from `read_channel_boot`: the
   ticket file's size (how many tickets), its console ID next to
   `ES_GetDeviceID`, its common key index, and whether the title is in
   an emulated NAND.
2. **Pass one ticket.** In `read_channel_boot`
   (`nandchannels.cpp:457-464`), keep the ticket whose console ID is this
   console's (or the first, when none is personalised), 0x2A4 bytes, and
   log which one was used.
3. **Try every d2x slot for channel games.** For a channel, build the
   list from `d2x_slots()` rather than `image_cios_order`: the slot with
   the TMD's base first, then the others, 248 included. A channel needs
   no USB or disc reads, so the image games' slot rules do not apply.
   Keep Nintendo's required IOS last, for Dolphin and for the log.
4. **Don't turn on NAND emulation where it cannot work.** Skip
   `enable_d2x_saves` on an IOS that is not d2x. Say plainly when no d2x
   slot could mount the card ("no d2x cIOS here could read the SD card,
   which an emulated NAND game needs"), so the player sees that cause
   and not the ES message.
5. **The own-NAND fallback.** For a game in the Wii's own NAND, check in
   the run that `StartChannel`'s `WII_LaunchTitle` fallback is reached
   and starts the game. If ES_Identify can't work on a vWii's d2x, that
   path, the title's own IOS with `ES_LaunchTitle`, is the way the game
   plays, without RiftWii's extras.
6. **An emulated NAND game without d2x's emulation.** No fallback is
   possible today. One would need the title copied to the real NAND,
   which RiftWii does not do and should not do without asking, so the
   message in proposal 4 is the honest answer until a vWii run shows
   which d2x slots can mount the card.

## What a vWii tester's run must show

From `sd:/riftwii/boot.log` of one launch of each reported game:

- "Wii channels: <ID> starts with RiftWii's loader: ... it asks for
  IOS<n>, tried: ...": the whole IOS list.
- For each IOS in it: "IOS<n> could not be started: ...", or the "Saves:
  d2x NAND emulation from ...: FAT module <a>, mount <b>, set <c>" line
  (emulated NAND only), and "on IOS<n>, ES would not run RiftWii as
  <ID> (<code>)" or "ES runs RiftWii as ..." (the codes per slot are the
  key evidence).
- "Wii channels: starting <title>" and, for the Wii's own NAND, whether
  "RiftWii's loader did not start it ...; the system starts it" follows
  and the game then starts.
- With proposal 1 in place: each IOS's revision and d2x base, and the
  ticket's size, console ID against the console's, and common key index.

Also: whether the game is in the Wii's own NAND or in `sd:/nands/...`
(Home's **WII** or **SD** badge), and whether it starts from the Wii Menu.

Sources: wiibrew.org's `/dev/es`, Ticket and TMD pages (public
documentation of ES and the file formats); libogc's `ES_Identify`,
`IPC_ENOENT` and `WII_LaunchTitle`. No Riivolution or other loader's
source was used.
