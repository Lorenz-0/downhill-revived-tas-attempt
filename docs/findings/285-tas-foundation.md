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
- [ ] the three checks above on a machine with the toolchain and the disc.

## What a movie is

Text, one field a line, `#` comments; `<field> <b0> <rx0> <ry0> <lx0> <ly0> ... <b3> <rx3> <ry3>
<lx3> <ly3>` (21 columns). `field` is `kernel_vsync_count` (decimal, strictly increasing),
buttons are the `PAD_*` bitmask in hex, sticks are 0-255 with 128 centred. A field with no pad
read writes no line; a field the movie lacks plays a neutral pad.

## Decisions

- **Offline only.** In an online race the local player's pad never reaches `host_pad_script`
  (`online.c` reads the device for it), so a movie could only ever drive players 2-4 while
  `pads_read` kept capturing all four. A half-applied movie is worse than none, and online
  gameplay is not this lane's to change: an online session turns playback and recording off at
  its first field, with one line, before any pad of that field is read.
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

## Result

Not merged yet. The unit compiles on its own and the hooks' call sites type-check; the game-level
checks in "Goal and acceptance" have not been run, because this lane has no toolchain, no disc
and no GPU in the environment it was written in. Nothing in `src/` changed.
