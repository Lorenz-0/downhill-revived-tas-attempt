/* Tool-assisted speedrun (TAS) movie record/playback and frame control.
   DHD_TAS_PLAY=<path>   - play back a movie file
   DHD_TAS_RECORD=<path> - record inputs to a movie file
   DHD_TAS_PAUSE=1       - start paused (frame advance mode)
   DHD_TAS_STEP=<path>   - step-control file: create/touch to advance one frame while paused,
                           or write "run"/"pause"/"step"/"quit" commands.
   Movie format (v1, text): lines of
     <field> <b0> <rx0> <ry0> <lx0> <ly0> <b1> <rx1> <ry1> <lx1> <ly1> <b2> <rx2> <ry2> <lx2> <ly2> <b3> <rx3> <ry3> <lx3> <ly3>
   where field is kernel_vsync_count, buttons are hex (PAD_* bitmask), sticks are 0-255. */
#ifndef TAS_H
#define TAS_H

#include "hle.h"

void tas_init(void);
int tas_playing(void);          /* 1 when a TAS movie is being played */
int tas_recording(void);        /* 1 when recording */
int tas_paused(void);           /* 1 when TAS execution is paused */
void tas_frame_advance_wait(void); /* blocks while paused until a step is requested; pumps host */
int tas_pad(int player, HostPad *out); /* fill pad from movie; 1 if provided */
void tas_capture(int player, const HostPad *out); /* capture final pad for recording */
void tas_vsync(void);           /* end-of-field: write recorded frame, advance state */

#endif
