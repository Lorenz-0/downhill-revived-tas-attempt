/* Tool-assisted speedrun (TAS): per-field input movie record/playback and frame advance.
   Player 1 only, offline only: the movie drives and records pads_read's player 0 (the main rider)
   and pads 2-4 keep the device and the scripts; an online session turns playback and recording
   off (see tas_frame_advance_wait).

   DHD_TAS_PLAY=<path>   - play a movie file (a file that cannot be read, or holds no field,
                           turns playback off: nothing is neutralized, the pad falls back to
                           the controller and to DHD_NAV / DHD_AUTOPAD / DHD_RACE_PAD)
   DHD_TAS_RECORD=<path> - record the input the game consumes to a movie file (truncated at start)
   DHD_TAS_KEYS=1        - the TAS's keys, live in any run. Without it they are live when one of
                           the variables above is set (a TAS session), and =0 turns them off in
                           one. C, Space and 1 (either of them) release exactly one field while
                           the game is paused, on the key's way down only: holding one down gives
                           one field, and SDL's key repeats give none. P pauses a running game and
                           releases a paused one, so neither a pause nor its release needs the
                           control file. While the keys are live they belong to the TAS: the
                           game's own keyboard layout never sees them (keyboard.c's keyboard_pad),
                           so C is not the kick and Space is not the pedal in a TAS session.
   DHD_TAS_PAUSE=1       - start paused. Needs DHD_TAS_STEP (or the keys) to release a field:
                           without either the game runs unpaused, and so does a host whose sleep
                           does not block (the browser)
   DHD_TAS_STEP=<path>   - the control file, read once a field and deleted after every command:
                             empty file or "step"  - while paused: run exactly one more field
                                                     while running: ignored (no frame is owed later)
                             "run" / "resume"      - while paused: run free
                             "pause"               - while running: pause before the next field
                             "quit"                - end the game, as EXIT GAME's Yes does
                           Any other content is refused and logged once.

   Movie format (v1, text): one field per line, '#' starts a comment line,
     <field> <b0> <rx0> <ry0> <lx0> <ly0> <b1> ... <b3> <rx3> <ry3> <lx3> <ly3>
   field is kernel_vsync_count (decimal, strictly increasing), b is the PAD_* bitmask in hex,
   sticks are 0-255 decimal (128 centred). Player 2 is p1, and so on. Only p0 is applied and
   recorded; a file's p1..p3 columns are parsed and checked, and ignored, so a single-player TAS
   never takes a second pad from its own source.
   Whole or nothing: one line that is not exactly these 21 columns (an extra column too), a field
   that is not digits only or does not strictly increase, a button word past 4 hex digits, a
   stick past 255, a truncated line, a read error, or an allocation that fails part way refuses
   the whole movie, and none of it is played. The log names the file, the line and the fault, then
   says `play movie '...' refused: playback off` once.

   Timing: a TAS run samples the pad at the VBLANK, the point pad_vsync() fills the frame the
   game reads, and never through the just-in-time refresh (pad.c) that a live player's input
   takes at the end of the frame wait: what is recorded is exactly what the game consumed, and
   playback feeds the same point. A TAS run therefore plays with one field more input latency
   than a hand-played one, in both directions, which is what keeps the two consistent.
   Use DHD_FRAME_CLOCK=1 for a repeatable run: normal play steps the game by the wall time since
   the last field, so a movie replayed in a paced session is not the same run, and tas_init says
   so once when it is not set. A pause is host time, never game time: when it releases, the game
   clock is rebased (as the port does for an online lobby wait), so paused seconds cost no field. */
#ifndef TAS_H
#define TAS_H

#include "hle.h"
#include "../include/guest.h"     /* GUEST: the claim's words are guest memory */
#include "../runtime/memory.h"    /* PS2_TAS_KEYS */

void tas_init(void);               /* once, from the first field; reads the variables above */
int tas_playing(void);             /* 1 when a movie loaded and is being played */
int tas_recording(void);           /* 1 when recording (the movie file is open) */
int tas_paused(void);              /* 1 when frame advance is holding the game */

/* The TAS's keys, as the game's keyboard sees them. tas.c publishes a session's claim as a bitmap
   in the HLE window (PS2_TAS_KEYS, port/runtime/memory.h) and this reads it, so the answer crosses
   the wasm boundary as state rather than as a call into the module: keyboard.c is compiled on both
   sides (port/tools/host64_census.py's HOST_UNITS and build_wasm.py's module units), and its host
   copy is the one that turns a real key into player 1's pad. A host-side tas.c would be a second
   instance with its own (never initialized) session and would answer 0 for ever. */
static inline volatile uint8_t *tas_claim_words(void)
{
    return (volatile uint8_t *)GUEST(PS2_TAS_KEYS);
}

/* 1 when a TAS session holds this key: the game's layout never sees it then. */
static inline int tas_key_claimed(unsigned vk)
{
    const volatile uint8_t *words = tas_claim_words();
    return vk < 256 && ((words[vk >> 3] >> (vk & 7)) & 1u) != 0;
}
void tas_frame_advance_wait(void); /* start of a field: pauses, polls the control file, refuses online */
int tas_pad(int player, HostPad *out);  /* player 0 only: fill the pad from the movie; 0 otherwise */
void tas_capture(int player, const HostPad *out); /* player 0 only: keep the pad this field consumed */
void tas_vsync(void);              /* end of a field: write the recorded field */

#endif
