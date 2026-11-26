# 286: savestates for the TAS — what a snapshot has to be here, and what it would cost

Status: in progress

Branch `arena/82a67d5f-downhill-revived-tas-attempt` from `main` 15f0ccc, 2026-11-26. This file is a
feasibility study: it decides nothing by itself and **implements nothing**. Its job is to turn "a
savestate would be nice" into a list of state, a measured obstacle, a cost, and four experiments
whose results decide whether the thing is worth building and in what order.

It follows finding 285, which ended with the roadmap this continues ("The road to savestates and
rewind"): the TAS foundation is in place — per-field movies, pause, frame advance, the keys — and a
savestate is the next thing that unblocks rerecording.

## Goal and acceptance

What a savestate is for, in this port's own terms: **save the machine at a field, and load it back so
that continuing from it is indistinguishable from never having stopped.** The use that matters first
is rerecording: hold the game at field N, save, try inputs, and if the run goes wrong load N back
instead of replaying 4000 fields. Rewind is the same machinery driven by a ring of checkpoints.

A savestate is accepted when:

- a run restored at field N and continued to field N+600 has the **same state trace**
  (`DHD_STATE_TRACE`, `state_diff.py`) as a run that reached N+600 without saving — every section,
  every field;
- the picture after a load is the picture at N (pixel-equal, or within the tolerance the renderer's
  own comparisons already use);
- the sound after a load resumes where it was, with no restart of a stream (`bench_race.py --sound`
  counts the gaps);
- it refuses what the TAS refuses: an online session, a build that is not the one that wrote it, and
  a load that disagrees with the movie's field.

Out of scope here: implementation, a rewind UI, cross-build savestates, savestates in online play or
over a replay, and anything that edits guest memory by hand (the movie already covers input).

## What "the state" is here, measured

The inventory, from the code. "In the trace" is whether `state_trace.c` hashes it at the field
boundary; a savestate has to carry everything the simulation can see, which is a superset.

| block | where | size | in the trace | copyable as-is |
| --- | --- | --- | --- | --- |
| EE RAM | PS2 0, host `0x08000000` (`memory.h`) | 32 MiB | `ram` | yes — plain memory at a fixed host address |
| HLE window | `0x70004000`, 16 KiB | 16 KiB | no, by design | yes, but it is derived: the TAS's claim (`PS2_TAS_KEYS`), the disc's recent lookups, the SIF packets |
| scratchpad | `0x70000000` | 16 KiB | `scratch` | yes |
| EE thread stacks | guest `0x06000000`, 64 × 256 KiB (`fiber.c`) | 16 MiB | pointers into it are normalised, the stacks themselves are not | yes — fixed guest addresses, so a copy back needs no fixups |
| CPU registers | `ps2_cpu ps2` (`ps2asm.h`) | ~2.1 KiB | `cpu` | yes — its two pointers point at VU0's memory at fixed guest addresses |
| suspended threads' registers | `threads[256].regs` (`kernel.c`) | ~170 KiB | **no** — HLE host state | yes |
| VU0/VU1 memory and VU1's register file | `0x11000000`, 64 KiB | 64 KiB + regs | `vu0` | yes — `host_vu1_snapshot` already hands VU1's registers over |
| GS: 4 MiB software VRAM, registers, privileged registers, CLUT/CBP, paths, the vertex pipeline | `Gs` prefix (`gs.h`) | 4 MiB + ~4 KiB | `gs_regs`, `gs_vram`, `gs_stream` | yes — `host_service_gs_snapshot` already copies the whole prefix into guest memory |
| sound driver: voices, streams, banks, duckers, reverb and grain state, `rendered_samples`, `vsync_count` | `sound.c` | tens of KiB | **no** — only the field's rendered PCM | yes; the audio device's buffers are re-created and re-fed |
| kernel HLE: threads, semaphores, alarms, `intc_stat`, `next_vsync`, `heap_end`, `gs_imr`, DMA and SIF registers | `kernel.c`, `dma.c`, `sif.c` | ~200 KiB | **no** | yes |
| padman: `pads[2][4]`, `frame_counter` | `pad.c` | small | **no** (the frames are in RAM) | yes |
| CDVD: open files, the streaming LSN | `cdvd.c` | small | **no** | re-open by path |
| graphics queue: the ring and its marks | `graphics_queue.c` | host | **no** | re-fed from the GS state |
| presentation pacing: the pace grid, the presented pictures' statistics | `host.c`, `pace_grid.h` | host, wall clock | **no** | rebased, as a released pause already does |
| TAS: playing/recording, the pause, the playback cursor, the claim | `tas.c` | small | **no** | yes |
| native stacks | one per active fiber, an 8 MiB mapping each (`fiber.c`), used part only | typical ≪ 1 MiB | **no**, deliberately | same process, or a re-map at the same address |

Sum of the guest-visible blocks: **≈ 36.2 MiB**, plus ~0.4 MiB of HLE state and the used part of the
native stacks. That is not a guess — every number is a `#define` or a `sizeof` in the files named.

Two facts make this list shorter than it looks:

- **The renderer and the audio device are not state.** Both are re-creatable from the GS prefix (VRAM
  and registers) and the sound driver's voices. The exact renderer can already replay a GS stream
  (findings 025, 085), and the queue is a pipe, not a store.
- **Guest addresses are fixed in every build** (`memory.h`), so a restore is a `memcpy` back and no
  pointer anywhere needs fixing — except the host addresses in the code-pointer words, which already
  have an identity mechanism (`codeids.py`) and are, like a movie, tied to the build that wrote them.

## What already exists, and is the reason this is a study and not a research project

- `host_service_gs_snapshot(address, bytes)` (`port/gs/gs.c`) copies the entire `Gs` prefix — 4 MiB of
  VRAM, every register, the paths — into guest memory, asserting the size with `offsetof(Gs, sync)`.
  That is half the savestate, in the shipping tree, working, for the trace.
- `host_vu1_snapshot(address)` (`port/hle/vif.c`) does the same for VU1's register file.
- `state_trace.c` is a **field-by-field, build-independent definition of what counts as game state**,
  with `DHD_STATE_DUMP` writing the raw section bytes out and `state_diff.py` comparing two runs. It
  is exactly the instrument the acceptance above needs, and it already waits for the graphics workers
  before it hashes (a checkpoint must be taken at that same quiet point, and by default it is the
  default).
- `fiber.c` already keeps, per slot, the three values a native-stack copy needs: the saved native
  stack pointer, the module's stack pointer and the call depth. The mapping's base is in the record.
- The clock is deterministic on the fixed step (`DHD_FRAME_CLOCK=1`), which is what makes
  checkpoint-plus-replay exact instead of approximate. A movie's replay equality is the same
  prerequisite and is still an open check in finding 285 — it should be closed before this stage.

## The obstacle, stated exactly

The state trace is a **map**, not a save: it hashes what the game can see and deliberately omits two
layers that a restore still needs.

1. **The live frames.** At a field boundary every guest thread is suspended inside an HLE wait; the
   locals and return addresses of the recompiled C functions on the way there live on that thread's
   *native* stack, which is host memory, and the guest stack pointer is a register in HLE state. The
   trace normalises a word that points into the EE stacks (`DHD_STATE_NORM=fibers`) and never hashes
   the stacks' contents, because two compilers lay those frames out differently and the trace must be
   comparable across builds. A savestate has no such luxury: it must copy the stacks.

   Why it is tractable anyway: the guest stacks sit at fixed guest addresses (64 slots of 256 KiB), and
   each native stack is a mapping of fixed size whose used bytes are `saved pointer − base` — bounded,
   and restorable in the same process into the same mapping. A cross-process restore needs the mapping
   re-created at the same address, which `VirtualAlloc`/`mmap` do with a fixed hint.

2. **The HLE's own bookkeeping** — ~0.4 MiB of host C state: 256 threads' saved registers, semaphores,
   alarms, timers, `intc_stat`, `next_vsync`, the DMA and SIF registers, padman, the sound driver, the
   disc's positions, the graphics queue's marks, the pace grid, and the TAS's own cursor. None of it is
   in RAM and none of it is traced. Restoring RAM alone resumes with stale threads and stale timers —
   which is precisely the failure the bisection experiment below is designed to locate, layer by layer.

## Cost, and the shape of rewind

- A checkpoint is ≈ 36.2 MiB uncompressed before the native stacks. **Compression is the open
  number** (experiment E1): PS2 RAM compresses well in general, but "well" is not a number, and
  12 MiB is the threshold that makes a disk-backed ring practical.
- **Replay is fast enough to be the rewind mechanism.** This port's own benches (docs/STATE-LOG.md)
  measure 121–329 fields/s on desktops and 49–70 on phones *with the picture being drawn*; a replay
  with no picture is at least that. So five seconds of run (300 fields) costs ≈1–2.5 s of replay on a
  desktop and ≈5–6 s on a phone.
- Therefore the shape is the one finding 285 named: **checkpoints plus replay, not a snapshot per
  field.** A checkpoint every ~5 s of run, a ring of a handful on disk, and rewind = "load the nearest
  earlier checkpoint, replay to the target". A savestate file is one checkpoint plus the field it
  belongs to and the boot it was taken on.

## The experiments that decide it

Each is cheap, each has a gate, and together they answer "worth building, and in what order".

**E1 — what a checkpoint costs on disk.** Take one field's sections with `DHD_STATE_DUMP` (the
machinery exists), concatenate them as a savestate would, and compress with zstd and lz4. *Gate:
≤ 12 MiB.* If it fails, the design becomes "one in-memory checkpoint, no ring", which still supports
rerecording but not deep rewind.

**E2 — how much native stack is really live.** At the trace point, log, per active fiber, `saved
pointer − base`, summed and maximum. *Gate: ≤ 512 KiB typical, with the theoretical bound (64 × 8
MiB) recorded as the worst case a pathological run could reach.*

**E3 — the decisive one: restore and continue.** A throwaway loader behind `DHD_STATE_LOAD=<prefix>`
that memcpy's a dump's guest sections back at the trace point and continues, compared with
`state_diff.py` against the original run. Then bisect the layers, one at a time, until 600 fields are
equal:

1. RAM, scratch, `ps2`, VU, the GS prefix — one `memcpy` each, all of it already copyable;
2. the HLE layer: the threads' saved registers and the kernel's counters;
3. the EE stacks (guest, 16 MiB area, used part per thread);
4. the native stacks.

*Gate: every section equal for 600 fields.* If an early step already diverges, its `state_diff.py`
report names the field and the section, and *that* is the finding.

**E4 — sound and picture after a load.** `bench_race.py --sound` before and after a restore: the gaps
must not show a restarted stream, and the picture at the load field must match.

E3 is the only one that can surprise: it is possible that at a field boundary the live frames matter
much less than expected (the game's threads are all parked in an HLE wait whose continuation is in
RAM), and it is possible they matter exactly as much as finding 285 expects. Either way the answer is
one run and one diff away, and it decides how much of this is a weekend and how much is a project.

## Risks, in the order they will bite

1. **The native stacks** (E3.4): the largest unknown, and the only part with no precedent in the tree.
2. **The sound driver** (E4): invisible to the trace until someone listens; voices in the wrong place
   sound immediately wrong, which makes it cheap to catch.
3. **The graphics queue and the renderer**: a load mid-race must not present a frame built from the
   previous state; the queue is re-fed from the GS prefix, but the mailbox's marks must be reset.
4. **The pace grid and the field clock**: a load must rebase the clock exactly as a released pause
   does, or the first field after a load runs at the wrong speed.
5. **The code-pointer words** in RAM: a savestate is tied to the build that wrote it, exactly as a
   movie is (`codeids.py` identities make the *comparison* portable; a restore needs the addresses).
6. **Online**: refused, as the TAS already refuses it, and for the same reason.

## Stage 1, first cut: save and load in a session (this commit)

The authorisation for "save anywhere / load anywhere" arrived, so the first cut is in the tree:
`port/hle/savestate.c` and its header, a state function pair in `kernel.c` and `pad.c`, and one new
host service (`host_service_gs_restore`, the exact inverse of the snapshot `gs.c` has given the trace
all along).

**What a savestate contains:** RAM, the scratchpad, the HLE window, VU0/VU1's memory, the CPU's
registers (block `cpu`), the kernel's own state (256 threads with their saved registers, 512
semaphores, the alarms, the interrupt handlers and counters, the fields counted) and padman's.
**≈32.3 MB**; the whole file is checked — magic, version, block
count, every block's name and size, and the file's own length — **before one byte of the machine is
written**, so a file that is not this state is refused whole.

**Two saves on Windows ended the process. The second one moved the whole unit onto ground it can
stand on:**

- **The EE thread stacks** (16 MiB at 0x06000000) were in the block list, copied whole. The host
  commits those slots page by page, per thread, with a guard page each
  (`host64_memory_commit_stack`), so most of that area had never been committed and reading it is the
  guest's own fault — the process ends, no dialog, exactly as a wild access does. They are out.
- **The GS image** was a 4 MiB buffer. As a static it does not fit the module's data (the wasm link
  refuses it); as the heap's it means growing the module's memory into a 4 MiB run, which is not
  memory this unit can count on being committed — and that save died too. The GS is **streamed** now:
  one 64 KiB static piece at a time, straight into (or out of) the file, through two new piece
  services (`host_service_gs_image`, `host_service_gs_image_write`, the offset forms of the snapshot
  and restore). Nothing in this unit is allocated and nothing is bigger than 64 KiB plus the kernel's
  and padman's own images (≈205 KiB and 4 KiB, static).

The keys are **Ctrl+5** (save) and **Ctrl+8** (load) rather than F5/F8, so a plain press of a key the
game may want stays the game's, and the edge is only taken with Ctrl held.

**The crash the third log finally showed, and its cause.** The log's last lines were

    savestate: write ram     33554432 bytes
    host64 trap: Out-of-bounds access in linear memory or a table (EE fiber 1, wasm depth 0)

— the save died inside the first block. The block was `GUEST(0)` 32 MiB, which is PS2 address 0 …
except that this unit is *module* code, and in the module PS2 address 0 is at **PS2_RAM_BASE
(0x08000000)**, not at 0: the first 4 MiB of the module's memory is wasm-ld's shadow stack
(`--stack-first`, committed by `host64_memory_commit_stack`) above a null guard page at [0, 0x4000),
and the low RAM *view* starts at 0x00400000. A copy from 0 therefore reads the shadow stack and then
the uncommitted guard page, and wasm2c's bounds check ends the process with an out-of-bounds trap —
the same class of mistake as the EE stacks (reading a region because its address looked right), one
layer down. The block now reads `PS2_RAM_BASE`, which is what `state_trace.c` has always used for
RAM (`PS2_PTR`, `PS2_SCRATCHPAD`), and the scratchpad goes through `PS2_SCRATCHPAD` by name.

**And the GS is not in a savestate (it was, for two builds).** Its 4 MiB prefix went in through the
snapshot service after `host_service_graphics_wait()`, and that wait is the one thing a save does that
a menu save does not need: the port draws on worker threads and defers the vsync (finding 090 HO1),
so a wait for the drawing from inside the field's vblank is a synchronous stall in a pipeline that
the *next* field is meant to overlap — `state_trace.c` says as much and offers `DHD_STATE_NOWAIT=1`
to skip it. The first save, in the main menu, worked and loaded (field 1602, 36.3 MB, every block);
the second, in a race, never returned: no `savestate: saved` line, and the freeze log's dump put the
game thread outside the exe with the same translated-code frames as the earlier trap. So the block is
gone, with the wait, and the unit is what its own rule says the renderer is — not state: RAM, the
scratchpad, the HLE window, VU memory, the CPU, the kernel and padman, ≈32.3 MB, no host call in the
save path at all but the file itself. The GS comes back when there is a point in the field where the
workers are quiet by construction, not when one can be waited for.

**The race save's next log says it is the RAM write.** With the GS and its wait gone, the same
report came back: `savestate: write ram 33554432 bytes`, then `freeze: no field for 5 s at field
7084: menu 9 phase 1` — `0x29DED8` is 9 in a race and the phase 1 is the countdown (finding 024), so
this is a race, at the first block, with no trap this time but a hang. The same 32 MiB write in the
main menu had already worked (field 1602), and a menu save does not differ from a race save in
anything else this unit does. So: the write is now a megabyte to a piece, with a breadcrumb per
piece, which says how far into RAM a run got (and bounds each `fwrite`); `DHD_QUICKSAVE=<path>` puts
the file elsewhere, so a save on a local disk rules the game's own drive out (a network or virtual
one, the one the report's build folder is on); and the game thread's frames in the freeze dump want
naming with `port/tools/host64_rva.py <map> <log>`, which says whether the hang is in the file write
or in a wait. The TAS keys, separately, were never broken: they are the game's unless a session
claims them (mission 7's rule), and the first field now says so — `DHD_TAS_KEYS=1` claims them.

**The breadcrumb a report brought was one line:** `save: block ram 33554432 bytes` and nothing else,
so the save stops inside the first block. Its build was older than the megabyte pieces, so it cannot
say whether the write was stuck or slow. The next build answers that three ways: every line carries
the wall-clock seconds it was written (a slow write and a stuck one look alike otherwise, and the gap
between two lines is the call between them); the write is a megabyte a piece with a line each;
and `DHD_SAVE_SMALL=1` leaves RAM out, which is the one test that separates "the big write hangs"
from "something else does" — the state it writes cannot be loaded back, and says so.

**What the first two logs a report came with actually contained** (worth keeping, because between
them they show what a missing line means): one ended `exit game: chosen in the main menu`, `end:
closing (code 0)` and the session's `perf:` line — the port's graceful exit, no savestate line
anywhere; the other was nine lines long, `Downhill Revived: No disc image is chosen.` and `end:
closing (code 0)` — a launch with no disc chosen, the game never booted, so the module (and this
unit with it) never ran. Neither was a crash, and neither is the report's crash: the file a crash
writes is `dhd.log` **of that run**, and its own line first (`Downhill Revived 0.8.13 (dhd-port …
<hash>)`, a different hash every build).

So this unit also keeps `savestate-trace.txt` beside the game: one line per action, the file opened
and closed per line, so the last line is on disk before anything can end the process. `boot: …`
means the build has this and reached the first field; a `key: Ctrl+5` line without a `save:` line
means the key was read; `save: block ram 33554432 bytes` with nothing after it means the process
ended inside that block; no `boot:` line at all means the exe is not this build (or the game never
started).

**And the third "the game dies" report was not a die at all.** Its log ended
`game: the front end runs`, `exit game: chosen in the main menu`, `end: closing (code 0)` and the
whole session's `perf:` line after it — the port's own graceful exit, with no savestate line
anywhere. The keys were behind the TAS session's gate (a movie, a control file, `DHD_TAS_KEYS`, or a
`DHD_SAVE_STATE`/`DHD_LOAD_STATE`), no session was live, so Ctrl+5 was never read; what the log
recorded was the player leaving the menu afterwards. The gate is gone: the keys save and load in any
session, and are refused only while an online race is (the TAS's own rule). `savestate_init` also
says the keys' names on the first line of every session now, so a report's log says which build made
it and whether this feature is in it — the absence of that line, not a crash, is what a stale build
looks like.

**The first save on Windows ended the process, and why is worth keeping:** the block list included
the 64 EE thread stacks, 16 MiB at 0x06000000, copied whole. The host commits those slots page by
page, per thread, with a guard page each (`fiber.c`, `host64_memory_commit_stack`), so most of that
16 MiB had never been committed and reading it is the guest's own fault — the process ended, no
dialog, exactly as a wild access does. The stacks are out of the list: they are the continuation of
suspended threads, the same class as the live frames below, and putting them back properly needs a
host service that copies one slot's used pages rather than an address range. The first save and the
first load now write one line per block (`savestate: write ram 33554432 bytes`, ...) so that a run
that dies in one of them says which.

**How it is used:** F5 saves to `dhd-quicksave.state` and F8 loads it back, live in a TAS session (a
movie, a control file, `DHD_TAS_KEYS`, or a savestate variable of its own), so an ordinary player's
F5/F8 stay the game's. For a scripted run, `DHD_SAVE_STATE=<file>` (with `DHD_SAVE_FIELD=<n>` to
choose the field) and `DHD_LOAD_STATE=<file>`. Both are taken and applied at the field boundary the
state trace hashes at, after the graphics work the field started has been waited for.

**What is knowingly missing, in the order it will show:**

- **the live frames** — the recompiled code's own C frames and each fibre's native stack. They are
  host memory a fresh boot has not got, which is why a load is supported *within the session that
  saved*: F5, play, F8, retry — the rerecording loop, which is what a savestate is for here. A file
  loaded into a fresh boot will restore the machine and then resume on the new process's frames: it
  may work, it may diverge, and the log says so instead of pretending.
- **the renderer's caches and the audio device**: the GS is back, but the native renderer may hold a
  stale texture or pipeline for a field until the game redraws; the sound driver's own state is not
  yet in the file (the voices restart). Both are named in the load's log line.
- **open files and the disc's position** are left alone (they are the same files).
- **the field counter is restored with the state**, so a trace comparison of "load at N and run 600
  fields" against "run to N+600" is a straight diff (`DHD_STATE_TRACE`, `state_diff.py`).

**One build failure, and what it taught:** the first cut held the GS image as a 4 MiB static, and
the wasm link refused it — `wasm-ld: error: initial memory too small, 1897499920 bytes needed`. The
core's data image runs from 0x70200000 to about 0x70D9C000 and its initial memory ends at 0x70E00000
(`build_wasm.py`), so there is roughly 800 KB of room for new statics in the whole module, not 4 MB.
The images are the heap's now, allocated at the first save and sized by their owners, which is the
same place `state_trace.c` puts the packet it hands to `host_service_gs_trace_pop` — so nothing new
is in the module's data at all, and the initial memory is untouched.

**Verified here:** `tas_selftest.py` **112 checks, 0 failed** — four new, from `test_savestate.c`,
which drives the same reader and writer the game does over three small blocks and refuses four wrong
files (another size, another name, a short file, not a savestate) while proving the targets are
untouched. Compiles clean with `-Wall -Wextra` on the host and for `wasm32` (`savestate.c`,
`kernel.c`, `pad.c`); the browser's graphics reactor folds the new guest address
(`build_graphics.py`).

**Not verified here, and it is the whole question:** whether a load continues the same run. That
needs the game, and the honest test is one sentence long — trace a run, save at a field, load, run
600 fields, and diff the two traces. The layers most likely to show up in that diff are the two
above, and each has a next step ready (native stacks through a host service; the sound driver's
tables, unit by unit as `kernel.c` and `pad.c` did).

## Decision requested

Nothing here is built. Three stages, each needing its own authorisation:

- **Stage 0 — measurement only (throwaway).** E1, E2 and the E3 bisection behind `DHD_STATE_LOAD`,
  in the port tree, not shipped, no UI. Output: the numbers, the layer list, and a yes or no.
- **Stage 1 — save and load (product).** Done as the first cut above (F5/F8 and
  `DHD_SAVE_STATE`/`DHD_LOAD_STATE`); what remains is what its first run's diff says, and the
  sound driver's state if audio matters to the loop.
- **Stage 2 — rewind.** The checkpoint ring and replay behind it, once Stage 1's equality holds.

Recommendation: Stage 0 first. It is small, it answers the only questions that matter, and it produces
the two numbers (E1's size, E3's layer list) that make Stage 1 estimable instead of optimistic.

## What can and cannot be checked where

Checked while writing this: the inventory (every size is a `#define` or a `sizeof` in the named file),
the existing snapshot paths (`gs.c`, `vif.c`), the trace's sections and dump option, the fibre record,
the fixed guest addresses, and the throughput figures quoted from docs/STATE-LOG.md.

Not checked, and not checkable in this lane: anything that needs the game — booting it, running a
race, taking a real dump, compressing it, or any of E1–E4. This lane has no disc, no GPU and no
Windows toolchain; every experiment above needs the user's PC.
