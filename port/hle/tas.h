/* Tool-assisted speedrun (TAS): per-field input movie record/playback and frame advance.
   Offline only: an online session turns playback and recording off (see tas_frame_advance_wait).

   DHD_TAS_PLAY=<path>   - play a movie file (a file that cannot be read, or holds no field,
                           turns playback off: nothing is neutralized, the pad falls back to
                           the controller and to DHD_NAV / DHD_AUTOPAD / DHD_RACE_PAD)
   DHD_TAS_RECORD=<path> - record the input the game consumes to a movie file (truncated at start)
   DHD_TAS_PAUSE=1       - start paused. Needs DHD_TAS_STEP: without it the game runs unpaused,
                           and so does a host whose sleep does not block (the browser)
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
   sticks are 0-255 decimal (128 centred). Player 2 is p1, and so on.
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
   Use DHD_FRAME_CLOCK=1 for a repeatable run. */
#ifndef TAS_H
#define TAS_H

#include "hle.h"

void tas_init(void);               /* once, from the first field; reads the variables above */
int tas_playing(void);             /* 1 when a movie loaded and is being played */
int tas_recording(void);           /* 1 when recording (the movie file is open) */
int tas_paused(void);              /* 1 when frame advance is holding the game */
void tas_frame_advance_wait(void); /* start of a field: pauses, polls the control file, refuses online */
int tas_pad(int player, HostPad *out);  /* fill the pad from the movie; 0 if it has none */
void tas_capture(int player, const HostPad *out); /* keep the pad this field consumed */
void tas_vsync(void);              /* end of a field: write the recorded field */

#endif
