/* The grid paced frames keep (kernel.c pace(), host.c host_pacing_wait's slow stretch): each frame
   is due one period after the one before, so frames that come in quick repay a slow one, and the
   grid starts again from now once a frame is more than two periods behind it.

   The period is not a constant of the session (fps.c port_frame_period: 1/60 from a race's load to
   GO, a replay's recorded step every update): a change re-anchors the grid at the last frame's
   place, so the frames before keep the period they had. Multiplying all of them by the new one
   moved the due time by frames x the change, a wait of seconds (144 to 60 after 1000 frames:
   9.7 s). */
#ifndef DHD_PACE_GRID_H
#define DHD_PACE_GRID_H
#include <stdint.h>

typedef struct {
    double base;      /* the grid's start (0: none yet) */
    double period;    /* the period of the frames since base */
    uint64_t frames;  /* frames since base */
} PaceGrid;

/* The next frame's due time, or 0 when the grid starts again from `now` (no wait). */
static inline double pace_grid_next(PaceGrid *grid, double now, double period)
{
    if (period != grid->period) {
        if (grid->base) grid->base += (double)grid->frames * grid->period;
        grid->frames = 0;
        grid->period = period;
    }
    double due = grid->base + (double)++grid->frames * period;
    if (!grid->base || now > due + 2 * period) {
        grid->base = now;
        grid->frames = 0;
        return 0;
    }
    return due;
}

/* The due time a free-running producer's picture is handed to the presenter with (host.c's
   host_pacing_handoff, the presentation pacing of a deterministic run). Never 0 — a 0 due means
   "present at once" and skips every other check — and never in the future, because the presenter
   is the grid that cannot drift: it shows a picture no sooner than one field after the one
   before (`pace_show_at`), counted from when the last picture actually went out, not from how
   many fields the producer has made.

   A *field* grid is what went wrong (finding 285, revision 7: the screen went uncapped while the
   run itself was fine). It gave each field a slot, so for a game that makes hundreds of fields a
   second the due ran further and further ahead of the wall clock; the presenter's catch-up reach
   (a due more than 50 ms away is dismissed, so a stalled queue never holds a run) then threw the
   hold away on every picture, and the pictures went out as fast as the machine drew them. */
static inline double pace_present_due(double now)
{
    return now > 0 ? now : 1e-9;   /* never the 0 that means "at once" */
}

/* When the presenter may show a picture that is ready now, given its due time (0: at once, which
   the caller then does not ask about), the time the last picture went out (0: none) and the
   least spacing between pictures. A due further away than `reach` is a producer far ahead of the
   screen: the picture goes out at once rather than stalling the run behind a queue. Returns the
   time to wait until, and `now` itself when nothing should hold the picture. */
static inline double pace_show_at(double due, double now, double last_at, double spacing, double reach)
{
    double target = due;
    if (last_at > 0 && last_at + spacing > target) target = last_at + spacing;
    if (target > now && target < now + reach) return target;
    return now;
}
#endif
