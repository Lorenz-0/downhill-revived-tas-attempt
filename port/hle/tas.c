/* Tool-assisted speedrun (TAS): per-frame input movie record/playback and frame control. */
#include "tas.h"
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern uint64_t kernel_vsync_count;

enum { TAS_PLAYERS = 4 };

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

typedef struct {
    uint16_t buttons;
    uint8_t right_x, right_y, left_x, left_y;
} TasPad;

typedef struct {
    uint64_t field;
    TasPad pads[TAS_PLAYERS];
    int has;
} TasFrame;

static int initialized;
static int playing, recording;
static int paused, step_requested;
static const char *play_path, *record_path, *step_path;

static TasFrame *movie;
static size_t movie_len, movie_cap;
static size_t play_cursor;

static TasFrame cur_rec;
static int cur_rec_field_set;
static uint64_t cur_rec_field;

static FILE *rec_fp;

static void tas_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "tas: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
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

static int parse_u16_hex(const char *s, uint16_t *out)
{
    char *e = NULL;
    unsigned long v = strtoul(s, &e, 16);
    if (e == s || *e) return 0;
    *out = (uint16_t)v;
    return 1;
}

static int parse_u8(const char *s, uint8_t *out)
{
    char *e = NULL;
    unsigned long v = strtoul(s, &e, 0);
    if (e == s || *e || v > 255) return 0;
    *out = (uint8_t)v;
    return 1;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *e = NULL;
    unsigned long long v = strtoull(s, &e, 0);
    if (e == s || *e) return 0;
    *out = (uint64_t)v;
    return 1;
}

static void movie_push(TasFrame f)
{
    if (movie_len == movie_cap) {
        size_t ncap = movie_cap ? movie_cap * 2 : 1024;
        TasFrame *n = (TasFrame *)realloc(movie, ncap * sizeof *movie);
        if (!n) { tas_log("out of memory loading movie"); return; }
        movie = n;
        movie_cap = ncap;
    }
    movie[movie_len++] = f;
}

static void load_movie(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0;
    if (!f) { tas_log("cannot open play movie: %s", path); return; }
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        char *tok[1 + TAS_PLAYERS * 5];
        int nt = 0;
        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#') continue;
        trim(p);
        for (char *t = strtok(p, " \t"); t && nt < (int)(1 + TAS_PLAYERS * 5); t = strtok(NULL, " \t"))
            tok[nt++] = t;
        if (nt == 0) continue;
        if (nt != 1 + TAS_PLAYERS * 5) {
            tas_log("%s:%d: expected %d columns, got %d", path, lineno, 1 + TAS_PLAYERS * 5, nt);
            continue;
        }
        TasFrame fr;
        memset(&fr, 0, sizeof fr);
        if (!parse_u64(tok[0], &fr.field)) { tas_log("%s:%d: bad field", path, lineno); continue; }
        int ok = 1;
        for (int i = 0; i < TAS_PLAYERS && ok; i++) {
            int base = 1 + i * 5;
            ok &= parse_u16_hex(tok[base + 0], &fr.pads[i].buttons);
            ok &= parse_u8(tok[base + 1], &fr.pads[i].right_x);
            ok &= parse_u8(tok[base + 2], &fr.pads[i].right_y);
            ok &= parse_u8(tok[base + 3], &fr.pads[i].left_x);
            ok &= parse_u8(tok[base + 4], &fr.pads[i].left_y);
        }
        if (!ok) { tas_log("%s:%d: bad value", path, lineno); continue; }
        fr.has = 1;
        movie_push(fr);
    }
    fclose(f);
    tas_log("loaded %zu frames from %s", movie_len, path);
}

void tas_init(void)
{
    if (initialized) return;
    initialized = 1;
    parse_path_env("DHD_TAS_PLAY", &play_path);
    parse_path_env("DHD_TAS_RECORD", &record_path);
    parse_path_env("DHD_TAS_STEP", &step_path);
    const char *pause = getenv("DHD_TAS_PAUSE");
    paused = pause && *pause && strcmp(pause, "0") != 0;
    if (play_path) {
        playing = 1;
        load_movie(play_path);
    }
    if (record_path) {
        rec_fp = fopen(record_path, "w");
        if (!rec_fp) {
            tas_log("cannot open record movie: %s", record_path);
        } else {
            recording = 1;
            fprintf(rec_fp, "# DHDR TAS movie v1\n");
            fprintf(rec_fp, "# field p0(btn rx ry lx ly) p1(...) p2(...) p3(...)\n");
            fflush(rec_fp);
            tas_log("recording to %s", record_path);
        }
    }
    if (paused) tas_log("paused at start (DHD_TAS_PAUSE=1)");
}

int tas_playing(void) { tas_init(); return playing; }
int tas_recording(void) { tas_init(); return recording; }
int tas_paused(void) { tas_init(); return paused; }

static void clear_step_file(void)
{
    if (step_path) remove(step_path);
}

static void poll_step_control(void)
{
    if (!step_path) return;
    FILE *f = fopen(step_path, "r");
    char cmd[64] = {0};
    if (!f) return;
    if (fgets(cmd, sizeof cmd, f)) trim(cmd);
    fclose(f);
    if (cmd[0] == 0 || !strcmp(cmd, "step")) {
        step_requested = 1;
        clear_step_file();
        return;
    }
    if (!strcmp(cmd, "run") || !strcmp(cmd, "resume")) { paused = 0; clear_step_file(); return; }
    if (!strcmp(cmd, "pause")) { paused = 1; clear_step_file(); return; }
    clear_step_file();
}

void tas_frame_advance_wait(void)
{
    tas_init();
    if (!paused) { poll_step_control(); return; }
    for (;;) {
        poll_step_control();
        if (step_requested || !paused) break;
        host_service_gs_pump_messages();
        host_sleep_ms(16);
    }
    if (step_requested) step_requested = 0;
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
    if (player < 0 || player >= TAS_PLAYERS) return 0;
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
    if (player < 0 || player >= TAS_PLAYERS) return;
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
    if (recording && rec_fp && cur_rec.has) {
        fprintf(rec_fp, "%llu", (unsigned long long)cur_rec.field);
        for (int i = 0; i < TAS_PLAYERS; i++) {
            TasPad *p = &cur_rec.pads[i];
            fprintf(rec_fp, " %04X %u %u %u %u", (unsigned)p->buttons, p->right_x, p->right_y, p->left_x, p->left_y);
        }
        fprintf(rec_fp, "\n");
        fflush(rec_fp);
        cur_rec.has = 0;
    }
}
