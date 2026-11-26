# 285: a TAS foundation — per-field input movies, pause and frame advance

Status: in progress

Branch `arena/82a67d5f-downhill-revived-tas-attempt` from `main` 15f0ccc, started 2026-11-25.
This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A foundation for tool-assisted speedruns in the port: one input movie a field for all four pads,
recorded from the input the game actually consumes and played back into the same point, plus
pause and frame advance for building a run by hand.

Acceptance, on a machine with the toolchain and the disc:

- `python tools/check.py quick`, and `check.py port` if the host side moved (it does not here).
- `python port/tools/tas_selftest.py`: the unit's own tests, on the host and without the game
  (below), 0 failed.
- Boot and race state traces equal to a base build's, with no `DHD_TAS_*` set: the hooks are inert.
- A recorded run and its playback give the same state trace (`DHD_FRAME_CLOCK=1` both times).
- By hand: a paused game steps exactly one field per `step`, `run` resumes, `quit` ends it.

Out of scope, and deliberately not started here: savestates, rewind, rerecording, an input
editor, a timeline, video capture.

## Progress

- [x] 2026-11-25: the foundation, recovered from the lane's design patch and applied
      (b0be1b9; its one stale context line, `present.c` for `presence.c`, corrected in
      `build_wasm.py` so the patch applies to 15f0ccc).
- [x] 2026-11-25: hardening of the failure paths, the pause, the recording and the logs
      (0661f0b).
- [x] 2026-11-25: critical review of the two commits above. The online guard had no
      `pads_online()` test at all, so it turned every offline TAS off at its first field; the
      pause was not covered online; the parser accepted too much. `tas.c` rewritten for the gate,
      the online pause and a strict whole-movie parser, and the regression suite added
      (`port/tools/tas_selftest.py`, 60 checks through the game's field order, 0 failed).
- [x] 2026-11-25: architectural audit against the port's own machine (below, "In this game's own
      terms"). Three narrow corrections: the movie drives and records player 1 only (it took pads
      2-4 from their own sources), `tas_init` says once when the run cannot be repeated (the game
      then steps by wall time), and a released pause rebases the game clock so paused seconds are
      never paid back as physics. No emulator assumption was found to remove: the hooks were
      already written against the game's own VBLANK, padman frames and libpad buffer.
- [x] 2026-11-26: by hand, on the user's Windows PC at `f612424`: `DHD_FRAME_CLOCK=1
      DHD_FRAME_PACE=1` holds the pictures at the field rate, P holds and releases the game, and
      C, Space and 1 each advance exactly one field and leave the game held. The first two runs
      found the two bugs of revisions 7 and 8 (the keys never crossing the boundary, and the step
      keys needing a pause first); both fixed, and this run is the confirmation.
- [ ] the checks that need a state trace against a base build: a recorded run and its playback
      giving the same trace, and the hooks inert with no `DHD_TAS_*` set (`DHD_STATE_TRACE`), plus
      `check.py port` for the host side.

## What a movie is

Text, one field a line, `#` comments; `<field> <b0> <rx0> <ry0> <lx0> <ly0> ... <b3> <rx3> <ry3>
<lx3> <ly3>` (21 columns). `field` is `kernel_vsync_count` (decimal, strictly increasing),
buttons are the `PAD_*` bitmask in hex, sticks are 0-255 with 128 centred. A field with no pad
read writes no line; a field the movie lacks plays a neutral pad.

**Whole or nothing.** The file is one statement of a run, so a file with anything wrong in it is
refused entire, before it can drive a field: not exactly 21 columns on any line (an extra column
is counted and named), a field that is not digits only (sign, `0x`, spaces, a value that overflows
64 bits), a field that does not strictly increase, a button word past 4 hex digits, a stick past
255, a read error, a file that cannot be opened, a file with no field, or an allocation that
fails part way (the loaded fields are freed with it). A line past the reader's 1024-byte buffer is
read in pieces: a piece with too many columns is refused like any other, and trailing blanks in a
second piece change nothing. The
refusal names the file, the line and the fault, then states the outcome once
(`play movie '...' refused: playback off`). Nothing is played from a refused movie, not even the
fields that did load; the pads fall back to the controller and to `DHD_NAV` / `DHD_AUTOPAD` /
`DHD_RACE_PAD`.

## In this game's own terms

This port runs the game's own code: the matching decompilation in `src/` compiled to a native
core, with `port/hle` standing in for the PS2 system (`docs/STATE.md`, "The port: how it runs the
game"). No console's frame boundary and no controller port are emulated in the sense a TAS has to
care about, and the TAS is written against these facts:

- **A field is the game's own VBLANK.** `kernel_vsync_count` (kernel.c) counts the VBLANK events
  the port delivers; the game ends each of its frames in `sceGsSyncV` (`func_00243CE8`) waiting
  for that event, and the port's `deliver()` raises it at the virtual cycle deadline. It counts
  the game's frames, the same way in every build, and it is the movie's key.
- **EE time is virtual already.** The EE's cycle clock is the port's `ee_frame_clock_*`
  (timer.c, kernel.c): a kernel boundary adds a fixed 1024 cycles and a wait advances to its
  deadline, so timers, alarms and the field spacing never see the host. The one wall-clock
  coupling left is the *physics step* (fps.c, `port_delta_clock`), which is why a movie needs the
  fixed step to be a repeatable run.
- **Input enters as the hardware's does.** `pad.c` is the IOP's padman: two 128-byte frames per pad
  in EE RAM, the older refreshed at each VBLANK from `pads_read`. The game's libpad copies the
  newer reply into its own `D_00660B80[id]` (`func_002451B0`), `func_0021F8E0` polls it once a
  frame, `func_00220230`/`func_00220280` read buttons and sticks, `func_002202F8` folds a light
  press's pressure into a button bit, and `func_00220640` hands the rider's controller
  (`func_0017DD70`) the bit, or the pressure while riding. Player 1 is `pads_read`'s player 0
  (port 0 slot 0, the pad the game's rider 1 reads).
- **The TAS sits where the port's own scripts do.** `host_pad_script` (autopad.c) is the scripted
  source above the device: `tas_pad` answers first, so a refused or absent movie leaves `DHD_NAV` /
  `DHD_AUTOPAD` / `DHD_RACE_PAD` and the controller exactly as they were. The capture sits in
  `pads_read`, after `held_back`: the pad padman will publish for that field.
- **Online is another input path.** In an online race player 0's pad comes straight from the device
  (online.c's `pads_read_now`), so a movie cannot drive it; `pads_online()` is the whole test, and
  it is true from `DHD_NET_*` or once the ONLINE menu's room starts.
- **A pause stops the game, not a rider.** The wait is inside `vsync()`, which `deliver()` calls:
  while it holds, no guest fiber runs, no timer is polled, no alarm fires and no DMA completes, so
  the whole simulation is frozen (the render thread keeps presenting the last picture).
- **A movie is input, never state.** It is played into a live run from boot; nothing here restores
  a machine's state, and nothing claims that replaying input alone reproduces a run from any other
  starting point (the "Surprises" note on the missing header, and the savestate section below).

## Decisions

- **Offline only, and only a real session says so.** In an online race the local player's pad
  never reaches `host_pad_script` (`online.c` reads the device for it), so a movie could only ever
  drive players 2-4 while `pads_read` kept capturing all four. A half-applied movie is worse than
  none, and online gameplay is not this lane's to change. The whole gate is one function,
  `online_blocks_tas()`, and its first line asks `pads_online()`: on a machine that never goes
  online it answers 0 and the TAS runs, which is the point. When a session is real it drops
  playback, recording (closing the file with the fields it has) and a pause, at the first field it
  is seen, **before any pad of that field is read** — `tas_frame_advance_wait()` is the first
  thing in `vsync()`, ahead of `pads_read` — and says each of those once. The refusal latches:
  leaving the session does not resume a half-played movie, a recording or a pause, and nothing is
  re-logged. (The first version of this gate refused on every field without ever asking
  `pads_online()`, which disabled every offline TAS at its first field: the reason the suite below
  drives the real entry point instead of the helpers.)
- **Player 1 only.** A single-player TAS drives one pad: `tas_pad` answers only for player 0 and
  `tas_capture` records only player 0, so pads 2-4 keep the device (a second human in a split
  race) and the scripts (a test). Before the audit a movie answered for all four pads and the
  recording kept all four, which took pads 2-4 from their own sources and recorded input the TAS
  does not use. The format keeps four pads' columns, so nothing about a file changes; a file's
  p1..p3 columns are parsed, checked, and ignored. `pad.c`'s just-in-time refresh stays off across
  the whole pad layer while a movie plays or records, as it already is for any scripted session
  (`DHD_NAV`, `DHD_AUTOPAD`, an online race): the input layer keeps the VBLANK timeline the movie
  is keyed on, and a second human in a split race would read at that timeline too, one field older
  than a live player's. That is the scripted layer's existing behaviour, not a TAS exception.
- **A run needs the fixed step, and says so.** Normal play steps the game by the wall time since
  the last field, a frame rate other than 60 or `DHD_STEP_CLOCK` / `DHD_STEP_PATTERN` by their own
  rules (fps.c); only the deterministic clock makes a movie replay as the run it recorded. With
  playback or recording on and the clock stepping any other way, `tas_init` says it once and
  names `DHD_FRAME_CLOCK=1`. It does not refuse: playing a movie in a paced session is still a
  coherent thing to watch, just not the same run.
- **A pause is host time, never game time.** While paused the host clock runs and the game does
  not, so the delta-time clock would pay the pause back as extra physics, up to the game's own
  two-field cap a step. The wait rebases the game clock when it releases (`port_clock_rebase`,
  the port's primitive for a wait that is no game time: an online lobby, an app returning from the
  background), so a paused second costs no field.
- **The movie plays where the recording samples: the VBLANK.** `pad_refresh` (pad.c) rewrites the
  frame with the device at the end of the pacing wait, for a hand player's latency, and it
  bypasses `pads_read`. Both directions therefore keep it off: the input a recording captures is
  the input the game consumed, and playback feeds that same point. A TAS run plays one field
  behind a hand-played one, in both directions, which is what keeps the pair consistent. Applying
  playback at the refresh point instead would need a second capture there; not worth it before
  savestates exist.
- **A refused movie costs nothing.** Unreadable, empty, or out-of-order movies leave playback
  off, so `host_pad_script` falls through to the device and to `DHD_NAV` / `DHD_AUTOPAD` /
  `DHD_RACE_PAD`, and `pad.c` keys the refresh off the TAS state rather than off `DHD_TAS_*` so a
  typo cannot silently change how a live player's pad behaves.
- **Pause needs two things, or it does not happen.** A control file to be released by, and a host
  sleep that really waits. `DHD_TAS_PAUSE` without `DHD_TAS_STEP` runs unpaused and says so;
  a host whose sleep does not block gets frame advance off rather than a spun event loop.
- **An online session is never held by frame advance.** Every way a pause can meet one is closed:
  `DHD_TAS_PAUSE=1` is dropped at the first field (after `tas_init` paused the game, the gate
  unpauses it); a `pause` command is refused instead of pausing (`'pause' refused: online
  session`); a session entered while paused — a paused game that joins from the ONLINE menu —
  breaks the wait on its next field, clearing the pause and a step it had granted; and the gate is
  re-asked on every pass of the paused loop, so a session that starts during a wait ends it too.
  The loop keeps servicing messages while it holds, as the game's own pause does.
- **Every command is consumed** (the file is deleted), so nothing can be replayed by a later
  pause, and a `step` seen while running is refused rather than kept for the next pause. `quit`
  ends the game the way EXIT GAME's Yes does (`host_service_gs_window_close`), not by exiting
  under the render thread (finding 210).
- **Recording is closed by `host_end_add`** and every write checked; a failed write ends the
  recording with the complete fields before it kept. This is the one module unit that uses it, so
  the registration lands on the module's `atexit` (run when `boot.c`'s `main` returns) rather than
  in the host's stages, before them; the per-field flush is what keeps the data safe either way.

## Surprises

- The browser's `host_sleep_ms` is a no-op (`port/web/www/worker.js`), so the pause loop that is
  correct on a desktop would spin the worker there and the page could never deliver a command.
  The host is asked once, by timing a 4 ms sleep, and frame advance stays off where it does not
  hold: browser frame advance needs an asynchronous architecture of its own, not this loop.
- The 21-column format is position-keyed and has no version, core id or start field, so a movie
  is only valid for the boot that produced it. That is fine while a run starts from boot; once
  savestates exist, a movie should carry the state it starts from.
- `DHD_AUTOPAD_FRAMES` already scripts buttons by field count, and `DHD_PUPPET_RECORD` /
  `DHD_PUPPET_PLAY` already record and replay riders. This lane adds what those cannot do (four
  pads with sticks, recording, pause and step); it does not merge them yet, and playback wins over
  the scripts while it is active.

- The unit's first tests called `tas_pad`, `tas_capture` and the parser directly, and all 54
  passed while the game could not run a TAS at all: the fault was in `tas_frame_advance_wait()`,
  the one entry point they never used. The suite now runs the game's own order — the wait, the
  field counter, the pad, the capture, the vsync — so a gate that is wrong where the game meets it
  fails a check. A test of this unit that skips the wait is not evidence.
- **Five bytes a pad are the whole input, pressure included.** The 12 pressure bytes a DualShock 2
  sends are folded into a button bit for a light press by the game (`func_002202F8`), and
  `func_00220640` returns them as a magnitude in riding mode, but every consumer of
  `func_00220618` in the game tests it for zero (`if`, `!= 0`, `!`), so a press is a press; the
  port's online path derives the same bytes from the buttons for the same reason (online.c). The
  movie's buttons and sticks are therefore the complete input, and playback's 255/0 pressures are
  what the game would have been given anyway. Nothing to store.
- A failed write of a field leaves it to the C library whether the line's bytes reached the file,
  so the log names only the fields complete before it; a half-written line makes the strict parser
  refuse that movie whole, which is the safe half of the same coin.

## The unit's own tests

`port/tools/tas_selftest.py` (and `docs/TESTING.md`, "The TAS unit") builds `tas.c` into a stub
host and drives it through the game's field order, one field at a time, with `pads_online()`,
`host_sleep_ms()`, the game clock, the keyboard's four TAS keys and the window close under the
test's control. 94 checks, 0
failed, seconds to run, a C compiler and nothing else; two of them link a fault-injection build (`--wrap=realloc`,
the movie chunk; `--wrap=fflush`, the third field write) and are skipped where the linker has no
`--wrap`. They cover, in the order the review asked for them: offline playback surviving the first
field, offline recording after initialization and its three fields, playback reproducing a
recording; an online session refusing playback (no pad of the field taken from the movie),
refusing recording (file closed, nothing in it); `DHD_TAS_PAUSE` and a `pause` command not
holding an online session; a paused session entering one and running on, with no pause left
behind; leaving a session with fields running, nothing paused and the refusal latched; and every
refusal a movie can get — 20 columns, an extra column, out of order, a repeated field, a negative
field, an overflowing field, a stick past 255, a button word past 16 bits, a truncated line at
either end, comments only, a missing file, an allocation failure — each leaving nothing playable.
It also holds the audit's three corrections: a movie answers for no pad but player 1's (and a
second pad's capture in the same field never reaches the file), the not-reproducible warning
appears exactly when the game steps by wall time and not otherwise, and a released pause rebases
the game clock once (never when no pause was held, never when the online gate released it).
Recording's file is checked closed with its fields when a session ends the TAS, when `quit` ends
the game and when the game ends normally, and a failed write ending it with its complete fields.

The harness is portable: its clock and sleep are POSIX `clock_gettime`/`nanosleep` on one side and
Windows `QueryPerformanceCounter`/`Sleep` on the other, with one check of that contract first (a
sleep that does not block would silently turn the unit's frame advance off), and the tests are the
same on both. Two families report SKIP rather than PASS where the toolchain cannot support them,
and the runner says so: the fault-injection builds need GNU ld's `--wrap` (MSVC's `link.exe` and
`lld-link` have no equivalent — seven checks), and Windows has no `/dev/full` (two checks).

## Controls and the screen

Two things a person at the keyboard could not do, and what each one cost to make possible without
touching the run a movie records.

**The TAS's keys.** C, Space and 1 release one field while the game is held; P holds a running game
and releases a held one. Read in `tas.c`, once a field and once a pass of the paused loop, from
`keyboard.c`'s key levels, and acted on only where a key goes down: the port's keyboard already
latches taps for the game's menus, but SDL hands a held key's OS repeats on as key-downs too, so a
tap latched by one of those would step a field every repeat — a level that is new is the only
honest "the player pressed this". While a TAS session is live the four keys are the TAS's:
`keyboard.c`'s `keyboard_pad`, the one place a layout's keys become a pad, drops a key the session
claims, so stepping a field is not also a kick (C is Circle) and a pause is not a pedal (Space is
Cross) — and it clears the key's own held state with it, so nothing is left held when the claim
ends. Outside a session, and with `DHD_TAS_KEYS=0`, nothing is claimed and the game's keys are
exactly as they were — a rule worth keeping, because those two keys are gameplay.

The claim crosses the wasm boundary as state, and this is the part worth writing down, because the
first attempt — let the host link `tas.c` too — was wrong in a way no test would have caught. The
native build keeps its own copy of `keyboard.c` (`port/tools/host64_census.py`'s `HOST_UNITS`), and
that copy is the one that builds player 1's pad, so it is the copy that has to know which keys are
claimed. But only the module has a session: `tas_init()` runs from `kernel.c`'s field, `pad.c`'s
reads and `online.c`'s gate, none of them host units, so a `tas.c` compiled into the host library
would be a second state machine whose `initialized` is never set — it would answer "not claimed"
for ever while the link closed cleanly, and the keys would quietly go back to kicking and pedaling
during a TAS. The link error was the visible half of the boundary being crossed in the wrong
direction: host code reaching into module code instead of module code publishing what the host
needs. `tas.c` therefore publishes the claim as a bitmap in the HLE window
(`PS2_TAS_KEYS`, 32 bytes, `port/runtime/memory.h` — scratch the game never reads, and the state
trace does not hash) at the moments the claim starts and ends (a session's start, an online
session's refusal, `tas_quit`), and `tas.h`'s `tas_key_claimed()` reads that bitmap: `static
inline`, the same answer in the module and in the host's copy, with no symbol for a linker to
argue about. `tas_selftest.py` links the real `keyboard.c` and checks the claim at the pad itself,
so the path this design depends on is the path that is tested.
The keys are also a release channel, which is what lets a pause exist without `DHD_TAS_STEP`; a
pause *at startup* still asks for one explicitly (`DHD_TAS_STEP`, or `DHD_TAS_KEYS=1`), so an old
command line that sets `DHD_TAS_PAUSE` alone keeps running instead of holding a windowless run
forever with no key to press.

**The pictures' pacing.** `DHD_FRAME_CLOCK=1` runs the game unpaced, and that is the point of it:
one fixed field per VBLANK, as fast as the machine makes them, is what makes the same movie the
same run. It also meant a TAS session presented several hundred pictures a second. The obvious fix
— stop taking `DHD_FRAME_CLOCK=1` to mean "unpaced" — is the wrong one, and worth writing down why:
`paced` in `kernel.c` is not a presentation switch. It also turns on `fps.c`'s wall-clock step (the
game would step by the time the field took, and a movie would no longer replay) and the
just-in-time frames (the pad is read at a different point in the field). So the pacing went where
the pictures are: `host.c`'s `pacing_present` already holds a picture to a due time, and
`host_pacing_handoff` now gives it a due from a grid of its own (`pace_grid.h`, the field period)
when the game is unpaced — `DHD_FRAME_PACE`, on by default for that case, `=0` for the old
behaviour. The frames behind the picture wait for the GS, not for a timer, so the game still runs
as fast as it can and the screen keeps 59.94 Hz; headless and bench runs have no pictures to pace
and are untouched. What stays identical: the fields the game sees, the field a movie names, the
pad read point, and the step.

## The road to savestates and rewind

Not started, and not to be started before the game-level checks below. What this game's
architecture says about it, from the audit:

**A savestate here is a copy of this machine, not of a console.** The state is: 32 MB of EE RAM at
a fixed host address (`memory.h`, `PS2_RAM_BASE`); the 16 MB of EE thread stacks
(`port/host64/fiber.c`: 64 slots of 256 KiB at 0x06000000); the 16 KB scratchpad; the EE's
hardware register pages and its CPU registers; VU0/VU1 memory (64 KB at 0x11000000) and VU1's
register file; the GS: its 4 MB software VRAM and its registers; and the HLE's own bookkeeping,
which is host C state and *not* in RAM — the kernel's threads (each fiber's saved module stack
pointer, call depth and native stack pointer), semaphores, alarms, timers (`cycle`, `remainder`),
`intc_stat` and `next_vsync`, the DMA channels, the SIF queues, padman's `pads[2][4]` and
`frame_counter` (the frames themselves are in RAM), the disc and file positions, the sound driver's
voices and mixer, the MPEG player, the GS queue and renderer mailbox, the pacing grid and frame
clock, and the TAS's own playback state while a movie runs. The player, rider, AI and race state
the user asks about is all in the first item: the riders are game objects in EE RAM (the online
puppets poke pose into the same objects, `puppet.c`), race phase and the shell's menu word are at
their own EE addresses (`0x43F348`, `0x29DED8`), and every random number generator is an LCG whose
state is a word in the game's data with a static seed (`D_0029D6D0`, `D_0029DA10`, `D_0029DAF8`,
`D_0029DB60`, `D_0029DCC4`), so a RAM copy carries all of it and nothing seeds from host time.

**What already exists to build on.** Guest addresses are fixed, so a restore is a copy back and
every guest pointer stays valid; `host_service_gs_snapshot` and `host_vu1_snapshot` already hand
the whole GS prefix and VU1's registers to the module (state_trace.c uses them); the state trace is
a per-field, build-independent definition of what counts as game state, with the code-pointer
identity mechanism (`port/tools/codeids.py`) for the words in RAM that hold host addresses; and
the clock is already deterministic on the fixed step, which is the same prerequisite the movie
needed.

**The obstacle that decides the design.** The recompiled game keeps live values in host C frames:
at a field boundary every guest thread is suspended inside an HLE wait, and the locals of the
functions on its way there are on the fiber's *native* stack, which is host memory. A checkpoint
that copies only guest memory resumes with stale locals. So a checkpoint must also copy each
active fiber's native stack (fixed size, its saved pointer is in the fiber record) and restore it
in the same process, into the same mapping. The trace deliberately ignores exactly this layer
("wall-clock and host state are never hashed"), so the trace is a map of the state, not a savestate.

**Route: checkpoints plus replay, not a snapshot per field.** 32 + 16 + 4 MB a field is no rewind
buffer. Because the simulation is deterministic on the fixed step, a checkpoint every N fields
plus the movie's inputs re-simulates any field between two checkpoints exactly. So a savestate is
a compressed checkpoint and the field it belongs to; rewind is "restore the nearest earlier
checkpoint, replay to the target"; and rerecording is the same replay that lets the movie's tail be
rewritten from the field where new input starts. That is the design this foundation makes possible,
and it is why a movie should then carry the checkpoint it starts from (the "Surprises" note about
the missing header).

**Named obstacles, in the order they will bite.** The native stacks above; the renderer's and audio
device's resources, which have to be re-created and re-fed from VRAM, registers and the mixer state
(the exact renderer can already replay a GS stream — findings 025, 085 — the same machinery); open
disc and file positions; online sessions, which must be excluded exactly as the TAS is; anything
that must survive a rebuild (the code-pointer identities); and the field/frame counters, which a
restore must put back or the game's own timers will see a jump.

**First experiment when the lane opens, and not before:** validate this foundation in the game,
then take one mid-race checkpoint of guest memory alone and diff what a restore gets wrong. That
diff is the honest scope of the savestate work.

## Revision 7: two things the first run on the user's PC found

Both were found by playing the build, not by the tests, and the tests are the part that changed
with them.

**The hotkeys did nothing.** C, Space, 1 and P reached `keyboard_key()` in the host's copy of
`keyboard.c` (SDL is the host's) and the TAS read `keyboard_position_down()` in the *core's* copy,
whose `position_down[]` no key event ever reaches: two copies of one file, one written, one read.
Revision 6's bitmap carried the *claim* the other way (the core telling the host which keys are
its), which is why nothing looked wrong. The levels now come the other way too, through two host
services that answer from the copy SDL writes: `host_service_keyboard_focused()` and
`host_service_keyboard_key(vk)` (`host_api.h`, `port/hle/host.c`, called once per read while a
session is live). No pointer crosses — a buffer would be a guest address the host's keyboard would
have to know how to write — and the core asks two questions per key instead.

The test is the build: `tas_selftest.py` compiles `keyboard.c` a second time with every public
name prefixed `host_` and links the harness with *only* that copy, its own imports answering from
it. The core then has no keyboard of its own at all, so a TAS that went back to reading one would
not link — and the same checks (P holds the game, C moves exactly one field, the game stays held)
pass in both builds. With no session, the harness's import counter proves the core reads no key.

**The pictures were still uncapped.** Revision 5 gave the presenter a *field grid*: one slot per
field, the due time the picture should be shown at. For a game that steps a field as fast as it can
make them, that grid runs away from the wall clock — hundreds of slots a second — and the
presenter's catch-up reach (a due more than 50 ms away is dismissed, so a stalled queue can never
hold a run) then threw the hold away on every picture: the screen went as fast as the machine drew
it while the run itself was perfectly fine. A 0 due did the same thing more simply (0 means
"present at once" and skips every other rule), and so did a spacing taken from the game's own
frame rate setting: a run fixed at 59.94 fields has 59.94 pictures to show, not 144.

Now the due a free-running picture is handed with is `now` — never 0, never ahead — and the thing
that paces the screen is the presenter's own hold: no picture sooner than one *field* after the one
before, counted from when the last picture actually went out (`pace_show_at`, `pace_present_due`,
`port/hle/pace_grid.h`). The field is 1001/60000 s however the game's frame rate is set, so
`DHD_FRAME_CLOCK=1` with `DHD_FRAME_PACE=1` shows 59.94 pictures a second with the fixed step
untouched (the sim is throttled by the picture queue, one field per picture, and a movie's fields
are numbered by the field, so the run is the same run). `DHD_FRAME_PACE=0` still hands the pictures
on at once for a throughput run, and `DHD_INTERP` still follows the display's refresh instead —
`kernel_init` now says so in `dhd.log` rather than leaving it to be discovered.

`DHD_PROFILE=1` (or `DHD_PACING_LOG`) now prints, every five seconds from the presenter:
`pacing: 59.94 pictures/s shown, 59.94 fields/s run, 16.68 ms held a picture` — the two numbers a
"the FPS is wrong" report has to tell apart, on the machine that has the screen.

Verified here: `tas_selftest.py` **104 checks, 0 failed**, of which six are the boundary build and
four are `test_pace.c`, which measures each of the three uncapped ways (500/s for a 0 due, 500/s
for a due far ahead, 144/s for a 144 Hz spacing) against the 59.94 the fix gives, and the same for
a run five times faster. Not verified here: any of it on a screen — the toolchain, the GPU and the
display are on the user's PC.

## Revision 8: the step keys needed a pause first

The first PC run after revision 7 had the frame rate capped and P working, and C, Space and 1 doing
nothing. They were read correctly — P is read by the same two imports, and revision 7's boundary
build is what proves the crossing — and then dropped: a step key granted a field only `if (paused)`,
and a running game is not paused. The rule existed for the *control file* (a `step` written while
the game runs must not be held for a later pause; it says so and refuses), where a command no one is
waiting on would move a field nobody asked for. For a key a person is pressing it is just a key that
does nothing, and nothing said so — and the first thing anyone does with a frame-advance key is
press it on a running game.

A step key now means the same thing from either state, which is the exchange itself: pressed on a
running game it holds at the boundary, so the field it is sitting on runs and the next one waits
(`tas: paused (C: this field runs, then the game is held; P releases)`); pressed on a held game it
releases exactly that one field and holds again (`tas: C: one field released (field 1234)`). P is
unchanged, and the file's `step` while running is still refused: a file is not a person.

Three things in `dhd.log` now answer "the keys do nothing" without another round trip: one line at
the first read with the host's own focus state (`tas: keys: the host reports focus=1` — a window
without the keyboard reports no key at all), a line per press with what it did, and the field number
the press moved. The held loop also looks at the keys every 2 ms rather than every 16, because a tap
shorter than the poll is a key that was never pressed.

Verified here: 108 checks, 0 failed, four of them new — a `step-run` scenario presses a step key on
a running game (one field runs, the game is held), schedules a release and a second press into the
held loop (exactly one more field), and reads both log lines. Not verified here: a screen and a hand.

## Result

Not merged yet. The by-hand acceptance — pause, frame advance, the keys, the displayed rate — passed
on the user's Windows PC at `f612424` (revision 8). Two checks that need a state trace against a base
build are still open (a recorded run replaying to the same trace, and the hooks inert without
`DHD_TAS_*`), and the merge is the user's decision.

Verified here: `tas_selftest.py` 94 checks, 0 failed (the two additions' checks: the four keys
claimed only in a session, P holding and releasing, C/Space/1 one field each, a held key giving
nothing over 31 passes of the paused loop, a paused start with and without a way out, and — with
the real `keyboard.c` linked — the claim's four bits in the window, a claimed key reaching no pad
button while an unclaimed one does, and the keys coming back with no session, with
`DHD_TAS_KEYS=0`, and when an online session ends one); `gcc -std=gnu99 -Wall -Wextra` clean on
`tas.c` and `-fsyntax-only` clean on the four hook sites (`pad.c`, `autopad.c`, `kernel.c`,
`online.c`); `tools/check_docs.py` and `tools/check_tree.py` clean.

On the user's machine this is confirmed by a fact that is easy to read the wrong way round: the
native link must **not** produce `port_hle_tas.c.obj`, and `host64.lib` must hold no `tas_` symbol
at all (`dumpbin /symbols build/host64.lib | findstr tas_` — nothing; before this change the same
command showed `keyboard.c` asking for `tas_key_claimed`). No host unit needs the TAS: the only
TAS name any of them mentions is `tas_key_claimed`, and that is a `static inline` in `tas.h` now.
A `port_hle_tas.c.obj` appearing in the archive would mean some host code still calls into the
module's TAS and the boundary has been crossed in the wrong direction again — a finding, not a fix.

Not verified here, and not claimable without them: the state traces of a base build and of this
one with no `DHD_TAS_*` set (the hooks are written to be inert, and this lane cannot trace), a
recorded run played back to the same trace, the keys as a person actually presses them (a tap
shorter than a poll is not seen: one field, 16 ms, while paused) and the pictures actually
arriving at 60 Hz — the disc, the toolchain and a GPU are on the user's PC, not here. The
presenter's half of the pacing (`host.c`, `present.c`) could not even be compiled in this lane: it
needs SDL3's headers and the render library. Nothing in `src/` changed.

The audit's verdict on the foundation: sound for this game, with no emulator assumption in it. Its
field key is the game's own VBLANK, its input enters where padman's frames are written and is
recorded where the game consumes it, its pause freezes the whole guest, and its online gate asks
the port's own `pads_online()`. The three corrections above were scope (player 1), honesty about
the clock, and the pause as host time; the remaining questions are game-level, not architectural.
