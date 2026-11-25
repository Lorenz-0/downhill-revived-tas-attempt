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
- [ ] the checks above that need the toolchain and the disc: a real run's state traces, and the
      game by hand.

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
- A failed write of a field leaves it to the C library whether the line's bytes reached the file,
  so the log names only the fields complete before it; a half-written line makes the strict parser
  refuse that movie whole, which is the safe half of the same coin.

## The unit's own tests

`port/tools/tas_selftest.py` (and `docs/TESTING.md`, "The TAS unit") builds `tas.c` into a stub
host and drives it through the game's field order, one field at a time, with `pads_online()`,
`host_sleep_ms()` and the window close under the test's control. 60 checks, 0 failed, seconds to
run, a C compiler and nothing else; two of them link a fault-injection build (`--wrap=realloc`,
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
Recording's file is checked closed with its fields when a session ends the TAS, when `quit` ends
the game and when the game ends normally, and a failed write ending it with its complete fields.

## Result

Not merged yet, and not to be merged before the game-level checks below are made.

Verified here: `tas_selftest.py` 60 checks, 0 failed; `gcc -std=gnu99 -Wall -Wextra` clean on
`tas.c` and `-fsyntax-only` clean on the four hook sites (`pad.c`, `autopad.c`, `kernel.c`,
`online.c`); `tools/check_docs.py` and `tools/check_tree.py` clean.

Not verified here, and not claimable without them: the state traces of a base build and of this
one with no `DHD_TAS_*` set (the hooks are written to be inert, and this lane cannot trace), a
recorded run played back to the same trace, and the game by hand — the disc, the toolchain and a
GPU are on the user's PC, not here. Nothing in `src/` changed.
