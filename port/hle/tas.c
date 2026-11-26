/* Tool-assisted speedrun (TAS): per-field input movie record/playback and frame advance.

   Offline only, by design. The pads_read hook captures whatever this game consumed, and in an
   online race the local player's pad never goes through host_pad_script (online.c reads the
   device for it), so playback would drive players 2-4 and not player 1: a half-applied movie
   is worse than none. An online session therefore turns playback, recording and frame advance
   off at the first field it is seen on (pads_online, online.c), before any pad of that field is
   read, with one line in dhd.log, and leaves the session untouched. Leaving a session does not
   bring a movie back: the refusal is latched, so a run never resumes from a half-played movie.

   Frame advance holds the game thread at the start of vsync() until the control file grants a
   field, so it needs a host sleep that really waits. The browser's host_sleep_ms is a no-op
   (port/web/www/worker.js): pausing there would spin the worker and never let the page deliver
   a command, so it is measured once (host_can_block) and frame advance stays off where it does
   not hold. An online session never holds the game either.

   A movie is taken whole or not at all: one line that is not exactly the 21 columns below, a
   value out of range, a field that does not strictly increase, or a load that cannot finish
   refuses the whole file. A deterministic run must not start from a movie the game only partly
   plays.

   Player 1 only (pads_read's player 0, the main rider): the movie drives that pad and records
   that pad, and pads 2-4 keep their own sources (the device for a second player, DHD_NAV /
   DHD_AUTOPAD / DHD_RACE_PAD for a test). The format still carries four pads' columns so a file
   needs no version for them; the other three are parsed and checked, and not applied.

   A movie is a run, and a run is only repeatable on the game's fixed step: normal play steps the
   game by the wall time since the last field (fps.c), so the same movie in a paced session is
   not the same run. With playback or recording on and the wall clock stepping the game, tas_init
   says so once (DHD_FRAME_CLOCK=1 keeps the fixed one-field step). A pause is host time only:
   when it releases, the game clock is rebased, as the online waits do, so the paused seconds are
   never paid back as extra steps of physics.

   The TAS's own keys (C, Space, 1 to step a field; P to pause and release) are read here, once
   per field and once per pass of the paused loop, on the key's way down only, so a held key and
   SDL's key repeats give one field at most. The levels come from the host
   (host_service_keyboard_focused and host_service_keyboard_key): SDL is the host's, and the host's
   copy of keyboard.c is where a key lands, so this module's copy of it stays blank (finding 285,
   revision 7). They are
   the TAS's while a session is live:
   keyboard.c's keyboard_pad asks tas_key_claimed() before a layout key becomes a pad button,
   so stepping a field is never also a kick. Outside a TAS session (and with DHD_TAS_KEYS=0)
   nothing is claimed and the keys are the game's as before.

   See tas.h for the movie format, the control file's commands and the input timing. */
#include "tas.h"
#include "hle.h"
#include "../host/host_end.h"
#include "../include/fps.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t kernel_vsync_count;

enum { TAS_PLAYERS = 4 };
enum { TAS_COLUMNS = 1 + TAS_PLAYERS * 5 };   /* field + (buttons, rx, ry, lx, ly) a player */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

typedef struct {
    uint16_t buttons;
    uint8_t right_x, right_y, left_x, left_y;
} TasPad;

typedef struct {
    uint64_t field;
    TasPad pads[TAS_PLAYERS];
    int has;                 /* the field was captured (cur_rec only; a movie frame is always set) */
} TasFrame;

static int initialized;
static int playing, recording;
static int paused;
static int step_granted;      /* one field released by the control file, cleared as it runs */
static int block_ok = -1;     /* this host can hold the game between fields; measured once */
static int online_refused;    /* an online session ended the TAS; said once, never undone */
static const char *play_path, *record_path, *step_path;
static int keys_on;           /* the TAS's keys are live this run (DHD_TAS_KEYS, or a session) */

static TasFrame *movie;
static size_t movie_len, movie_cap;
static size_t play_cursor;

static TasFrame cur_rec;
static int cur_rec_field_set;
static uint64_t cur_rec_field;

static FILE *rec_fp;
static unsigned long rec_fields;
static int end_registered;

static void tas_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    hle_log("tas: %s\n", line);
}

static void parse_path_env(const char *name, const char **out)
{
    const char *v = getenv(name);
    *out = (v && *v) ? v : NULL;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

/* The pad a movie drives and records: player 1, the main rider (pads_read's player 0, port 0 slot
   0, the pad the game's rider 1 reads). A single-player TAS takes no other pad: pads 2-4 keep the
   device and the scripts, so a second human in a split race and a test's script are untouched. */
enum { TAS_MOVIE_PLAYER = 0 };

/* The format has no sign and no prefix: digits only, and a value that fits. */
static int parse_u64_dec(const char *s, uint64_t *out)
{
    unsigned long long v;
    char *e = NULL;
    if (!*s) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    v = strtoull(s, &e, 10);
    if (*e || errno == ERANGE) return 0;
    *out = (uint64_t)v;
    return 1;
}

static int parse_u8_dec(const char *s, uint8_t *out)
{
    uint64_t v;
    if (!parse_u64_dec(s, &v) || v > 255) return 0;
    *out = (uint8_t)v;
    return 1;
}

/* One to four hex digits, no sign, no 0x: the PAD_* bitmask, as a recording prints it (%04X). */
static int parse_u16_hex(const char *s, uint16_t *out)
{
    unsigned long v = 0;
    int n = 0;
    if (!*s) return 0;
    for (const char *p = s; *p; p++, n++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return 0;
        if (n == 4) return 0;                    /* more than 16 bits */
        v = v * 16 + (unsigned long)d;
    }
    *out = (uint16_t)v;
    return 1;
}

static int movie_push(TasFrame f)
{
    if (movie_len == movie_cap) {
        size_t ncap = movie_cap ? movie_cap * 2 : 1024;
        TasFrame *n = (TasFrame *)realloc(movie, ncap * sizeof *movie);
        if (!n) return 0;
        movie = n;
        movie_cap = ncap;
    }
    movie[movie_len++] = f;
    return 1;
}

/* 1 when the whole movie loaded and is playable. Anything else refuses it whole: playback stays
   off rather than driving the game with a partial or empty movie, and the pad keeps its other
   sources. Every refusal names the file, the line and what was wrong with it. */
static int load_movie(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0;
    int ok = 1;
    uint64_t previous = 0;
    int have_previous = 0;
    if (!f) {
        tas_log("cannot read play movie '%s': playback off", path);
        return 0;
    }
    while (ok && fgets(line, sizeof line, f)) {
        char *p = line;
        char *tok[TAS_COLUMNS + 1];
        int nt = 0;
        int i;
        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#') continue;
        trim(p);
        /* One token more than the format allows is kept, so an extra column is counted and said. */
        for (char *t = strtok(p, " \t"); t && nt <= TAS_COLUMNS; t = strtok(NULL, " \t"))
            tok[nt++] = t;
        if (nt != TAS_COLUMNS) {
            tas_log("%s:%d: expected %d columns, got %d", path, lineno, TAS_COLUMNS, nt);
            ok = 0;
            break;
        }
        TasFrame fr;
        memset(&fr, 0, sizeof fr);
        if (!parse_u64_dec(tok[0], &fr.field)) {
            tas_log("%s:%d: bad field '%s'", path, lineno, tok[0]);
            ok = 0;
            break;
        }
        if (have_previous && fr.field <= previous) {
            tas_log("%s:%d: field %llu does not follow %llu", path, lineno,
                    (unsigned long long)fr.field, (unsigned long long)previous);
            ok = 0;
            break;
        }
        for (i = 0; i < TAS_PLAYERS && ok; i++) {
            int base = 1 + i * 5;
            if (!parse_u16_hex(tok[base + 0], &fr.pads[i].buttons)) {
                tas_log("%s:%d: player %d buttons '%s'", path, lineno, i + 1, tok[base + 0]);
                ok = 0;
            } else if (!parse_u8_dec(tok[base + 1], &fr.pads[i].right_x)) {
                tas_log("%s:%d: player %d right_x '%s'", path, lineno, i + 1, tok[base + 1]);
                ok = 0;
            } else if (!parse_u8_dec(tok[base + 2], &fr.pads[i].right_y)) {
                tas_log("%s:%d: player %d right_y '%s'", path, lineno, i + 1, tok[base + 2]);
                ok = 0;
            } else if (!parse_u8_dec(tok[base + 3], &fr.pads[i].left_x)) {
                tas_log("%s:%d: player %d left_x '%s'", path, lineno, i + 1, tok[base + 3]);
                ok = 0;
            } else if (!parse_u8_dec(tok[base + 4], &fr.pads[i].left_y)) {
                tas_log("%s:%d: player %d left_y '%s'", path, lineno, i + 1, tok[base + 4]);
                ok = 0;
            }
        }
        if (!ok) break;
        if (!movie_push(fr)) {
            tas_log("%s:%d: out of memory loading the movie", path, lineno);
            ok = 0;
            break;
        }
        previous = fr.field;
        have_previous = 1;
    }
    if (ok && ferror(f)) {
        tas_log("read error on '%s'", path);
        ok = 0;
    }
    fclose(f);
    if (!ok || !movie_len) {
        /* One statement of the outcome, after the reason: a movie is played whole or not at all. */
        if (ok) tas_log("play movie '%s' has no field", path);
        tas_log("play movie '%s' refused: playback off", path);
        free(movie);
        movie = NULL;
        movie_len = movie_cap = 0;
        return 0;
    }
    tas_log("playing %zu field(s) from '%s'", movie_len, path);
    return 1;
}

static void record_close(const char *why);

static void record_close_at_exit(void) { record_close(NULL); }

/* why != NULL: the session ended for that reason; NULL: the normal end. Either way the fields
   written so far stay in the file. */
static void record_close(const char *why)
{
    if (!rec_fp) return;
    if (why)
        tas_log("%s: %lu field(s) kept in '%s'", why, rec_fields, record_path);
    else if (fflush(rec_fp) != 0 || ferror(rec_fp))
        tas_log("recording ended with a write error: %lu field(s) kept in '%s'", rec_fields, record_path);
    else
        tas_log("recording ended: %lu field(s) in '%s'", rec_fields, record_path);
    fclose(rec_fp);
    rec_fp = NULL;
    recording = 0;
}

/* A failed write ends the recording instead of silently dropping the rest. Whether the bytes of
   the failing field reached the file is the C library's business, so the count names only the
   fields that were written and flushed before it; a line left half written makes the strict
   parser refuse the whole movie, so the next session never plays part of a damaged one. */
static void record_failed(const char *why)
{
    if (rec_fields)
        tas_log("recording stopped: %s on '%s' (%lu field(s) complete before it)", why, record_path,
                rec_fields);
    else
        tas_log("recording stopped: %s on '%s'", why, record_path);
    if (rec_fp) fclose(rec_fp);
    rec_fp = NULL;
    recording = 0;
}

static void record_open(const char *path)
{
    rec_fp = fopen(path, "w");
    if (!rec_fp) {
        tas_log("cannot write record movie '%s': recording off", path);
        return;
    }
    fprintf(rec_fp, "# DHDR TAS movie v1\n");
    fprintf(rec_fp, "# field p0(btn rx ry lx ly) p1(...) p2(...) p3(...)\n");
    if (fflush(rec_fp) != 0) {
        record_failed("cannot write the header");
        return;
    }
    recording = 1;
    rec_fields = 0;
    if (!end_registered) {
        end_registered = 1;
        /* Closed when the module's main returns (host_end.h's __wasm__ form is atexit: a module
           unit's registration runs before the host's host_end stages). Every field is flushed as
           it is written, so even a crash keeps the complete fields before it. */
        host_end_add(HOST_END_LOG, "tas movie", record_close_at_exit);
    }
    tas_log("recording to '%s'", path);
}

/* The browser's host_sleep_ms does nothing, so a paused loop there would spin the worker and
   the page could never write a command. A 4 ms sleep the wall clock does not see is no sleep.
   Measured the first time a pause is asked for, so a run that never pauses pays nothing. */
static int host_can_block(void)
{
    double before = host_wall_seconds();
    host_sleep_ms(4);
    return host_wall_seconds() - before >= 0.001;
}

/* The TAS's keys, by the place of the key on a US keyboard: the two ways to release a field and
   the one way to hold it. keyboard.c gives a letter by its place and the rest by the table of
   keys a binding can take (port/hle/controls_table.h); 0x20 and 0x31 are in it. */
enum { TAS_KEYS = 4 };
enum { TAS_KEY_PAUSE = 3 };   /* the other three are the ways to release a field */
static const unsigned tas_key_vk[TAS_KEYS] = {'C', 0x20, '1', 'P'};
static uint8_t tas_key_last[TAS_KEYS];   /* the level each key had when it was last read */

/* The keys belong to the TAS while a session is live (keyboard.c's keyboard_pad drops a claimed
   key, so C is not the kick and Space is not the pedal then). Outside one, and with
   DHD_TAS_KEYS=0, they are the game's keys as before: a player who is not running a TAS keeps the
   pedal on Space. The claim is published into the HLE window for the game's keyboard to read
   (tas.h tas_claim_words): keyboard.c is compiled for the host as well as for the module, and the
   host's copy is the one that builds player 1's pad. Called where the session's claim starts or
   ends, not once a field. */
static void tas_keys_publish(void)
{
    volatile uint8_t *words = tas_claim_words();
    for (unsigned i = 0; i < PS2_TAS_KEYS_SIZE; i++) words[i] = 0;
    if (!keys_on) return;
    for (int i = 0; i < TAS_KEYS; i++) {
        unsigned vk = (unsigned)tas_key_vk[i];
        words[vk >> 3] = (uint8_t)(words[vk >> 3] | (1u << (vk & 7)));
    }
}

static int can_pause_here(void);
static int online_blocks_tas(void);

static void tas_key_pause_toggle(void)
{
    if (paused) {
        paused = 0;
        tas_log("running (P)");
        return;
    }
    if (online_blocks_tas()) {
        tas_log("P refused: online session (TAS is offline only)");
        return;
    }
    if (!can_pause_here()) return;
    paused = 1;
    tas_log("paused (P: C, Space or 1 steps a field, P releases)");
}

/* A step key, from either state, is one field and then a held game: while the game is held it
   releases the field at hand, and while the game runs it holds at this boundary (the field it is
   sitting on runs, the next one waits) — which is the same exchange, and what a frame advance means.
   It used to be ignored unless the game was already held, so the keys read as "nothing happens"
   on the first PC run: the user had to know to press P first, and nothing said so. */
static void tas_step_press(const char *name)
{
    if (paused) {
        step_granted = 1;
        tas_log("%s: one field released (field %llu)", name, (unsigned long long)kernel_vsync_count + 1);
        return;
    }
    if (online_blocks_tas()) {
        tas_log("%s refused: online session (TAS is offline only)", name);
        return;
    }
    if (!can_pause_here()) return;
    paused = 1;
    tas_log("paused (%s: this field runs, then the game is held; P releases)", name);
}

/* One read of the four keys: only the ones that went down since the last read count. Levels, not
   the taps keyboard.c latches for the game's menus, because SDL hands a held key's repeats on as
   key-downs too, and a tap latched by one of those would step a field every repeat.

   The levels come from the host (host_service_keyboard_focused/key), not from keyboard.c's statics
   in this module: SDL runs on the host and its events go to the host's copy of keyboard.c, so this
   copy's position_down[] is never set and reading it made every key look up for ever (revision 7's
   bug: the hotkeys did nothing). Both calls are imports, only while a session is live. */
static void tas_keys_read(void)
{
    static const char *const names[TAS_KEYS] = {"C", "Space", "1", "P"};
    static int said_focus;
    int focused = host_service_keyboard_focused();
    if (!said_focus) {
        said_focus = 1;
        tas_log("keys: the host reports focus=%d (a window without the keyboard reads no key)", focused);
    }
    for (int i = 0; i < TAS_KEYS; i++) {
        int held = focused && host_service_keyboard_key(tas_key_vk[i]);
        int pressed = held && !tas_key_last[i];
        tas_key_last[i] = (uint8_t)held;
        if (!pressed) continue;
        tas_log("key %s down", names[i]);   /* the action logs itself below */
        if (i == TAS_KEY_PAUSE) tas_key_pause_toggle();
        else tas_step_press(names[i]);
    }
}

/* 1 when a pause may hold this host; 0 (with one line) when it may not. */
static int can_pause_here(void)
{
    if (block_ok < 0) {
        block_ok = host_can_block();
        if (!block_ok)
            tas_log("this build's host sleep does not block (the browser): frame advance is off");
    }
    return block_ok;
}

void tas_init(void)
{
    if (initialized) return;
    initialized = 1;
    parse_path_env("DHD_TAS_PLAY", &play_path);
    parse_path_env("DHD_TAS_RECORD", &record_path);
    parse_path_env("DHD_TAS_STEP", &step_path);
    const char *pause = getenv("DHD_TAS_PAUSE");
    int want_pause = pause && *pause && strcmp(pause, "0") != 0;
    const char *keys = getenv("DHD_TAS_KEYS");
    int keys_asked = keys && *keys && strcmp(keys, "0") != 0;   /* DHD_TAS_KEYS=1 and up */
    int keys_off = keys && *keys && !keys_asked;                /* DHD_TAS_KEYS=0 */
    keys_on = !keys_off && (keys_asked || play_path || record_path || step_path || want_pause);
    if (keys_on)
        tas_log("keys: C, Space or 1 steps a field and holds the game, P pauses and releases it "
                "(DHD_TAS_KEYS=0 turns them off)");
    else
        /* Silent before, which read as broken: the keys are the game's unless a session claims them
           (mission 7's rule), and a player practising alone has no session. */
        tas_log("keys: off, so C, Space, 1 and P are the game's (DHD_TAS_KEYS=1 claims them; a movie, "
                "a control file or DHD_TAS_PAUSE does too)");
    tas_keys_publish();
    if (play_path) playing = load_movie(play_path);
    if (record_path) record_open(record_path);
    /* A movie is a run: repeatable only on the game's fixed step. Normal play, a frame rate other
       than 60, DHD_STEP_CLOCK and DHD_STEP_PATTERN step the physics by something else (fps.c). */
    if ((playing || recording) && port_delta_clock())
        tas_log("this run is not reproducible: the game steps by wall time, not one fixed field "
                "(set DHD_FRAME_CLOCK=1 for a movie that replays the same)");
    if (want_pause) {
        /* A pause at the start needs a way out that is asked for: the control file, or the keys
           explicitly (DHD_TAS_KEYS). A session's keys alone are not enough for it, because a run
           with no window has no keys to press: an old command line that sets DHD_TAS_PAUSE and
           nothing else keeps running, as it always did, instead of holding forever. */
        if (!step_path && !keys_asked)
            tas_log("DHD_TAS_PAUSE needs DHD_TAS_STEP=<file> (or DHD_TAS_KEYS=1) to release a field: "
                    "running unpaused");
        else if (can_pause_here()) {
            paused = 1;
            if (step_path) tas_log("paused at the start; write step/run/pause/quit to '%s'", step_path);
            else tas_log("paused at the start; C, Space or 1 steps a field and P releases it");
        }
    }
}

int tas_playing(void) { tas_init(); return playing; }
int tas_recording(void) { tas_init(); return recording; }
int tas_paused(void) { tas_init(); return paused; }

enum TasCommand { TAS_NONE, TAS_STEP, TAS_RUN, TAS_PAUSE, TAS_QUIT, TAS_UNKNOWN };

/* The control file is consumed: every command is a fresh file, so nothing can be replayed by a
   later pause. */
static enum TasCommand read_command(void)
{
    FILE *f;
    char cmd[64] = {0};
    if (!step_path) return TAS_NONE;
    f = fopen(step_path, "r");
    if (!f) return TAS_NONE;
    if (!fgets(cmd, sizeof cmd, f)) cmd[0] = 0;
    fclose(f);
    remove(step_path);
    trim(cmd);
    if (!cmd[0] || !strcmp(cmd, "step")) return TAS_STEP;
    if (!strcmp(cmd, "run") || !strcmp(cmd, "resume")) return TAS_RUN;
    if (!strcmp(cmd, "pause")) return TAS_PAUSE;
    if (!strcmp(cmd, "quit")) return TAS_QUIT;
    tas_log("unknown command '%s' (step, run, resume, pause, quit)", cmd);
    return TAS_UNKNOWN;
}

static void tas_quit(void)
{
    tas_log("quit: the game ends, as EXIT GAME's Yes does");
    paused = 0;
    playing = 0;
    keys_on = 0;              /* the keys are the game's again */
    tas_keys_publish();
    if (recording) record_close(NULL);
    host_service_gs_window_close();
}

/* An online session forbids the TAS, before any pad of the field is read. It drops playback,
   recording and a pause the first time it is seen; the refusal is latched, so leaving a session
   never resumes a half-played movie or a silence the log no longer explains. */
static int online_blocks_tas(void)
{
    if (!pads_online()) return 0;
    if (!online_refused) {
        online_refused = 1;
        if (playing) {
            playing = 0;
            tas_log("online session: playback off (TAS is offline only)");
        }
        if (recording)
            record_close("online session: recording off (TAS is offline only)");
        if (paused) {
            paused = 0;
            step_granted = 0;
            tas_log("online session: frame advance off (TAS is offline only)");
        }
        keys_on = 0;          /* the keys are the game's again */
        tas_keys_publish();
    }
    return 1;
}

void tas_frame_advance_wait(void)
{
    int online;
    tas_init();
    online = online_blocks_tas();
    if (!paused) {
        switch (read_command()) {
        case TAS_PAUSE:
            if (online) tas_log("'pause' refused: online session (TAS is offline only)");
            /* With neither a control file nor the keys a pause could never be released: refuse
               it, never hang. */
            else if (!step_path && !keys_on)
                tas_log("'pause' needs DHD_TAS_STEP=<file> (or the TAS's keys): ignored");
            else if (can_pause_here()) { paused = 1; tas_log("paused"); }
            break;
        case TAS_QUIT: tas_quit(); break;
        case TAS_STEP:
            /* While running, a step is refused outright: holding it for a later pause would move
               a field nobody asked for at that pause. */
            tas_log("'step' ignored while running (write 'pause' first)");
            break;
        default: break;
        }
        if (keys_on) tas_keys_read();  /* P pauses; a step key now would move a field nobody asked for */
        return;
    }
    for (;;) {
        /* An online session never holds the game, even one entered while paused. */
        if (online_blocks_tas()) break;
        switch (read_command()) {
        case TAS_STEP: step_granted = 1; break;
        case TAS_RUN: paused = 0; tas_log("running"); break;
        case TAS_QUIT: tas_quit(); return;
        default: break;  /* already paused, or nothing new */
        }
        if (keys_on) tas_keys_read();
        if (step_granted || !paused) break;
        host_service_gs_pump_messages();
        /* 2 ms, not a field: a tap shorter than the poll would be missed between two looks, and a
           key that reads as never pressed is a frame advance that silently does nothing. */
        host_sleep_ms(2);
    }
    step_granted = 0;  /* exactly the field this call releases */
    /* The held time was host time: the game clock must not count it as steps of physics. The port
       calls this for its own waits that are no game time (an online lobby, an app returning from
       the background, fps.c). */
    port_clock_rebase();
}

static TasFrame *find_frame(uint64_t field)
{
    while (play_cursor < movie_len && movie[play_cursor].field < field) play_cursor++;
    if (play_cursor < movie_len && movie[play_cursor].field == field) return &movie[play_cursor];
    for (size_t i = 0; i < movie_len; i++) if (movie[i].field == field) return &movie[i];
    return NULL;
}

static void apply_pressures(HostPad *out)
{
    for (int i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
}

int tas_pad(int player, HostPad *out)
{
    tas_init();
    if (!playing) return 0;
    /* Only player 1: a movie never takes pads 2-4 from the device or from a script. */
    if (player != TAS_MOVIE_PLAYER) return 0;
    uint64_t field = (uint64_t)kernel_vsync_count;
    TasFrame *fr = find_frame(field);
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    out->connected = 1;
    if (fr) {
        TasPad *p = &fr->pads[player];
        out->buttons = p->buttons;
        out->right_x = p->right_x;
        out->right_y = p->right_y;
        out->left_x = p->left_x;
        out->left_y = p->left_y;
    }
    apply_pressures(out);
    return 1;
}

void tas_capture(int player, const HostPad *out)
{
    tas_init();
    if (!recording) return;
    if (player != TAS_MOVIE_PLAYER) return;   /* only player 1 is recorded; the others keep neutral */
    uint64_t field = (uint64_t)kernel_vsync_count;
    if (!cur_rec_field_set || cur_rec_field != field) {
        memset(&cur_rec, 0, sizeof cur_rec);
        cur_rec.field = field;
        cur_rec_field = field;
        cur_rec_field_set = 1;
        for (int i = 0; i < TAS_PLAYERS; i++) {
            cur_rec.pads[i].right_x = 128; cur_rec.pads[i].right_y = 128;
            cur_rec.pads[i].left_x = 128; cur_rec.pads[i].left_y = 128;
        }
    }
    cur_rec.pads[player].buttons = out->buttons;
    cur_rec.pads[player].right_x = out->right_x;
    cur_rec.pads[player].right_y = out->right_y;
    cur_rec.pads[player].left_x = out->left_x;
    cur_rec.pads[player].left_y = out->left_y;
    cur_rec.has = 1;
}

void tas_vsync(void)
{
    tas_init();
    if (!recording || !rec_fp || !cur_rec.has) return;
    if (fprintf(rec_fp, "%llu", (unsigned long long)cur_rec.field) < 0) {
        record_failed("write failed");
        return;
    }
    for (int i = 0; i < TAS_PLAYERS; i++) {
        TasPad *p = &cur_rec.pads[i];
        if (fprintf(rec_fp, " %04X %u %u %u %u", (unsigned)p->buttons, p->right_x, p->right_y,
                    p->left_x, p->left_y) < 0) {
            record_failed("write failed");
            return;
        }
    }
    if (fprintf(rec_fp, "\n") < 0 || fflush(rec_fp) != 0 || ferror(rec_fp)) {
        record_failed("write failed");
        return;
    }
    cur_rec.has = 0;
    rec_fields++;
}
