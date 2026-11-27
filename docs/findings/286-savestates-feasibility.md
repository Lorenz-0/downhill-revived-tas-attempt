# 286: savestates for the TAS — what a snapshot has to be here, and what it would cost

Status: in progress

Branch `arena/82a67d5f-downhill-revived-tas-attempt` from `main` 15f0ccc, 2026-11-26. This finding
began as a feasibility study: a list of state, a measured obstacle, a cost, and four experiments for
the later work. Stage 1 is now implemented on this branch and measured on Windows; E1–E4 and
Stage 2 remain future work, not something this cut quietly claims to have finished.

It follows finding 285, which ended with the roadmap this continues ("The road to savestates and
rewind"): the TAS foundation is in place — per-field movies, pause, frame advance, the keys — and a
savestate is the next thing that unblocks rerecording.

## Goal and acceptance

What a savestate is for, in this port's own terms: **save the machine at a field, and load it back so
that continuing from it is indistinguishable from never having stopped.** The use that matters first
is rerecording: hold the game at field N, save, try inputs, and if the run goes wrong load N back
instead of replaying 4000 fields. Rewind is the same machinery driven by a ring of checkpoints.

Stage 1 is accepted for this cut when:

- a real Windows save completes during an active race, and a load resumes that race in the same
  process;
- the main-menu save/load round trip still works;
- a malformed or incompatible file is refused before machine state is written;
- online races still refuse both keys; and
- the TAS keys still pause, release one field and resume during a race.

The full savestate acceptance remains ahead: a run restored at N and continued to N+600 must have
the same state trace as a run that never saved, the picture must match, and sound must continue
without restarting a stream. Build identity and a movie's playback cursor also need an explicit
policy before any cross-build or rerecording claim. Stage 1 does not claim those results.

Out of scope for Stage 1: a rewind UI, cross-build or cross-process loading, online play, the code's
live native frames, sound-driver state, GS/renderer state, and editing guest memory by hand (the
movie already covers input).

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
native stacks. That is the full inventory, not Stage 1's block list: the implemented seven-block
file is ≈32.3 MiB and deliberately omits several rows above.

Two facts matter when reading the list:

- **The renderer and audio device are outside Stage 1's file.** The full design could recreate the
  renderer from the GS prefix and audio from the sound driver's voices, but Stage 1 carries neither
  source yet. Picture and sound continuity therefore remain unverified, not assumed.
- **Guest addresses are fixed in every build** (`memory.h`), so a same-session restore can copy back
  without fixing guest pointers. Host addresses in code-pointer words remain build-specific; the
  identity mechanism (`codeids.py`) makes traces comparable, not savestates portable.

## What already exists, and why the full design is tractable

- `host_service_gs_snapshot(address, bytes)` (`port/gs/gs.c`) copies the entire `Gs` prefix — 4 MiB of
  VRAM, every register, the paths — into guest memory, asserting the size with `offsetof(Gs, sync)`.
  That is half the savestate, in the shipping tree, working, for the trace.
- `host_vu1_snapshot(address)` (`port/hle/vif.c`) does the same for VU1's register file.
- `state_trace.c` is a **field-by-field, build-independent definition of what counts as game state**,
  with `DHD_STATE_DUMP` writing the raw section bytes out and `state_diff.py` comparing two runs. It
  is the instrument the full acceptance above needs. It waits for graphics workers before hashing;
  that trace quiet point is broader than the narrow deferred-read settlement Stage 1 needs before
  copying RAM.
- `fiber.c` already keeps, per slot, the three values a native-stack copy needs: the saved native
  stack pointer, the module's stack pointer and the call depth. The mapping's base is in the record.
- The clock is deterministic on the fixed step (`DHD_FRAME_CLOCK=1`), which is what makes
  checkpoint-plus-replay exact instead of approximate. A movie's replay equality is the same
  prerequisite and remains an open check in finding 285; Stage 1 does not claim that result.

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

- A full-design checkpoint is ≈ 36.2 MiB uncompressed before the native stacks; Stage 1's measured
  seven-block file is 32.2 MiB. **Compression is the open number** (experiment E1): PS2 RAM compresses
  well in general, but "well" is not a number, and 12 MiB is the threshold that makes a disk-backed
  ring practical.
- **Replay is fast enough to be the rewind mechanism.** This port's own benches (docs/STATE-LOG.md)
  measure 121–329 fields/s on desktops and 49–70 on phones *with the picture being drawn*; a replay
  with no picture is at least that. So five seconds of run (300 fields) costs ≈1–2.5 s of replay on a
  desktop and ≈5–6 s on a phone.
- Therefore the shape is the one finding 285 named: **checkpoints plus replay, not a snapshot per
  field.** A checkpoint every ~5 s of run, a ring of a handful on disk, and rewind = "load the nearest
  earlier checkpoint, replay to the target". The implemented file holds one checkpoint and its field;
  it has no boot identifier and is constrained to the same session and build.

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

## Stage 1: same-session save and load

Stage 1 is implemented on this branch in `port/hle/savestate.c`, with state functions in `kernel.c`
and `pad.c`. It does not change the movie format, the simulation clock or the online race path.

### Format and guards

The file has seven blocks: RAM, scratchpad, the HLE window, VU memory, CPU registers, kernel state
and pad state. The measured file is **33,866,816 bytes (32.2 MiB)**. The header is 24 bytes and each
block's table entry is 28 bytes, so a seven-block file has 220 bytes before its data.

The reader checks the magic (`DHDSTAT1`), version, block count, every block name and size, and the
file's exact length **before it writes machine state**. A wrong size, wrong name, short file or wrong
magic is refused whole. The format has no independent build hash: it rejects incompatible layouts,
but cross-build loading is not promised. The saved field travels in the header and is restored; it is
not compared with the current field, because loading a checkpoint necessarily changes that field.
The movie's own cursor is not restored, and a live movie is warned about rather than called
reproducible.

Ctrl+5 saves and Ctrl+8 loads at a field boundary in any offline session, movie or not. Plain 5 and
8 remain the game's keys. `pads_online()` refuses both before a key is read, as the TAS refuses its
own controls online. Scripted runs can use `DHD_SAVE_STATE`, `DHD_SAVE_FIELD` and `DHD_LOAD_STATE`.

### What the Windows runs found

The first Windows save ended the process because the block list copied all 64 EE thread stacks. Those
slots are committed page by page and have guard pages, so reading the whole 16 MiB range touched
pages that had never been committed. The stacks are not in the file.

A 4 MiB static GS image then exceeded the module's data space. Streaming it worked in the main menu,
but the broad `host_service_graphics_wait()` used before the copy froze a race: the port defers the
vsync and overlaps drawing with the next field, so waiting there stalls the pipeline. The GS block
and that wait were removed. A later version briefly put the GS back; it is not in the current format.

The next crash was a plain address mistake. The RAM block read module address 0, but PS2 RAM begins at
`PS2_RAM_BASE` inside the module; address 0 is the shadow stack and guard area. The log ended with
`Out-of-bounds access in linear memory or a table`. RAM now reads `PS2_RAM_BASE`, and scratchpad uses
`PS2_SCRATCHPAD`.

The race freeze remained after those fixes. Timestamped, one-megabyte breadcrumbs showed the file
stop at exactly **7,340,252 bytes = 220 + 7 × 1,048,576**: the table and all seven RAM pieces had
been written, then the process stopped. Symbolizing the frozen thread with that run's map resolved
the instruction to **`NtWriteFile+0x14`**, a real write stall rather than a guessed slow disk. The
same full save froze on both the build volume and a local system disk. `DHD_SAVE_SMALL=1` completed,
and a full save with deferred reads disabled completed at the same field in about 0.016 seconds.

The difference was the RAM pages, not the file. Deferred GS store-image reads temporarily protect
their destination PS2 RAM pages with `PAGE_NOACCESS`. File I/O cannot take that fault and settle the
read, so passing those pages to `fwrite` stalled. `autopad.c`'s RAM dump and `cdvd.c`'s reads already
call `graphics_settle()` for this reason; the savestate path had omitted it.

### The fix

The fix is two calls, with no format change and no unconditional return to the broad graphics wait:

- save calls `graphics_settle()` before collecting the blocks, so every RAM page is readable;
- load calls it only after the whole-file metadata, block table, exact length and first-block seek
  have succeeded, immediately before reading block bytes into the machine.

That ordering keeps a refused file from settling deferred work or touching machine state. The save
path adds no unconditional rendering wait; it lets already-queued reads finish and restores the pages
they protected.

### Breadcrumbs

`savestate-trace.txt` keeps one line per action, with wall-clock seconds, a line per megabyte and a
line per block. The WASI adapter ignores append flags and currently truncates a writable create, so
opening the breadcrumb with `"a"` overwrote its own history. The file now stays open in `"w"` mode
for the session and flushes every line. A missing line therefore still identifies the call that did
not return, without losing the lines before it.

### Verification

`tas_selftest.py` reports **113 checks, 0 failed, 9 skipped** on Windows. The savestate harness uses
the real reader and writer, proves a valid load settles deferred reads once, and proves all four
wrong files are refused with **zero settlement calls and untouched targets**. The existing TAS
online tests still pass; `online_blocks()` itself is unchanged. `check.py port -j 16` passed every
step, including the renderer, network, launcher and stack-analysis checks.

The final race acceptance used the executable with SHA-256
`c6e263220e8c2ddfbcdadfd82aba7e6c2b229cf33b52f7de56d812520adc9971` and map SHA-256
`0338e78ec532b38908b1e9b20bb3fac00c41315626eeaca8c42ff0ea98d3b70a`.
The menu round trip and real refusal below were verified on an earlier build of the same source
change:

- **Main menu:** Ctrl+5 saved field 563 and Ctrl+8 loaded it in the same process. The file was
  33,866,816 bytes, the process stayed alive and its PID did not change. The breadcrumbs show about
  0.02 seconds for the save and 0.01 seconds for the load.
- **Race:** the scripted race saved at field 3815, 600 fields (10 seconds at 60) after GO. The file
  was 33,866,816 bytes, the save breadcrumb span was about 0.01 seconds and the process stayed alive.
  The race advanced to field 3840; Ctrl+8 was read at field 3892 and restored field 3815 in the same
  process. The next capture at field 3900 proved the race continued. P paused it; C, Space and 1
  released fields 3922, 3923 and 3924; P released the pause and the race advanced to field 3960.
- **No ordinary-game change:** deterministic base-versus-fixed traces printed PASS for all 600 boot
  rows and all 5000 race rows. The base executable was
  `50c49fdd052066edc82e39ce1561ef441bda09a08a1a713bb2a20f526ea9020c`.
- **Real refusal:** a valid state with one extra byte was offered to a fresh run. The log said
  `refused whole`, and its 600-row boot trace matched the untouched boot exactly.

### Known limits

- Loading is supported only in the session that saved. The code's live C frames and native stacks
  are not in the file, so a fresh process is not a supported restore.
- The GS, renderer caches and sound-driver state are not restored. A picture may be stale until the
  game redraws, and audio continuity has not been measured.
- Open files and the disc's position are left alone.
- The field counter is restored, but the full “load at N, continue to N+600, compare every section”
  acceptance has not been run. The successful race continuation above is not a claim of that
  stronger equality.
- The movie cursor is not restored. A recorded movie's trace equality with playback also remains
  the open check named in finding 285.
- E1–E4 (compression, live-stack size, layered restore equality, sound/picture continuity) remain
  measurements for later work.

### Later stages

Stage 1 is complete on this branch but not merged, so this finding remains `in progress`. Stage 2
— a checkpoint ring and replay-driven rewind — has not started and needs its own authorisation.
