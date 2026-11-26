/* The presentation pacing's arithmetic, without a screen: port/hle/pace_grid.h as host.c's
   host_pacing_handoff and pacing_present use it. Finding 285, revision 7: a fixed-step run's
   pictures reached the screen uncapped while the run itself was fine, and there are three ways
   that can happen — each one is a check here, so a change that reintroduces one fails.

     1. a due of 0, which the presenter reads as "present at once" and skips its every other rule
        with (revision 7's field grid answered 0 for any producer more than two slots ahead);
     2. a due far in the future, which the presenter's catch-up reach dismisses so a stalled queue
        never holds a run — a grid that counts one slot per field runs away from the wall clock
        when the run makes hundreds of fields a second;
     3. a spacing that is not the field's — the game's frame rate setting (144) rather than the
        fixed step's 59.94 — which shows 144 pictures of a 59.94-field run a second.

   With the fix, a producer of any speed shows 59.94 pictures a second. */
#include "pace_grid.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define FIELD (1001.0 / 60000.0)   /* 59.94 Hz: the field of a fixed-step run */
#define REACH 0.05                 /* pacing_present's catch-up reach (s) */

/* Pictures a second from a presenter that is handed pictures as fast as the producer makes them
   (`work` seconds of field each), shows them in order, and holds each one the way pacing_present
   does. `due_way`: 0 the fix (pace_present_due), 1 a due of 0, 2 a due far in the future. */
static double pictures_per_second(double spacing, int due_way, double work, int frames)
{
    double t = 100, last = 0, t0 = t;
    for (int i = 0; i < frames; i++) {
        t += work;                                    /* the field */
        double ready = t > last ? t : last;           /* in order: not before the one before it */
        double due = due_way == 0 ? pace_present_due(ready) : due_way == 1 ? 0 : ready + 100 * FIELD;
        double at = due > 0 ? pace_show_at(due, ready, last, spacing, REACH) : ready;
        assert(at >= ready - 1e-12);                  /* never sooner than the picture can be */
        last = at;                                    /* when it went out */
    }
    return frames / (last - t0);
}

int main(void)
{
    /* 0. the paced path's grid is untouched: the first field from now, then one period apart */
    PaceGrid paced = {0};
    double t = 5;
    assert(pace_grid_next(&paced, t, FIELD) == 0);
    double one = pace_grid_next(&paced, t, FIELD), two = pace_grid_next(&paced, t, FIELD);
    assert(fabs(one - (t + FIELD)) < 1e-12 && fabs(two - (t + 2 * FIELD)) < 1e-12);

    /* 1. the due a free-running picture is handed with: never 0, never ahead of now */
    for (int i = 0; i < 1000; i++) {
        double now = 10 + i * 0.001, due = pace_present_due(now);
        assert(due > 0 && due <= now + 1e-12);
    }

    /* 2. the screen keeps the field rate whatever the run's speed ... */
    double fixed = pictures_per_second(FIELD, 0, 0.002, 600);          /* 500 fields/s behind it */
    assert(fabs(fixed - 1 / FIELD) < 0.5);
    double slow = pictures_per_second(FIELD, 0, 0.0002, 600);          /* 5000 fields/s */
    assert(fabs(slow - 1 / FIELD) < 0.5);
    printf("pacing: %.2f pictures/s held to the field rate (a 5x faster run: %.2f)\n", fixed, slow);

    /* 3. ... and each of the three ways it did not: a 0 due, a due far ahead, a 144 Hz spacing */
    double at_once = pictures_per_second(FIELD, 1, 0.002, 600);
    double far = pictures_per_second(FIELD, 2, 0.002, 600);
    double as_144 = pictures_per_second(1.0 / 144, 0, 0.002, 600);
    assert(at_once > 400 && far > 400);                                /* uncapped, both */
    assert(fabs(as_144 - 144) < 1.0);                                  /* 144 pictures of 60 fields */
    printf("pacing: the three uncapped ways measured: %.0f/s (a 0 due), %.0f/s (a due far ahead), "
           "%.0f/s (a 144 Hz spacing)\n", at_once, far, as_144);

    /* 4. the catch-up reach: a due further off is not waited for, and one inside it is */
    double now = 20;
    assert(pace_show_at(now + 0.2, now, 0, FIELD, REACH) == now);
    double held = pace_show_at(now, now, now - 0.001, FIELD, REACH);
    assert(fabs(held - (now - 0.001 + FIELD)) < 1e-12 && held > now);

    puts("test_pace: ok (a free-running producer, 60 pictures a second, the three uncapped ways named)");
    return 0;
}
