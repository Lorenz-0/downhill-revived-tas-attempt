/* Integration-path tests for port/hle/tas.c: every field goes through the same order kernel.c
   and pads_read use — tas_frame_advance_wait(), ++kernel_vsync_count, the pad source
   (tas_pad when a movie plays, else "the controller"), tas_capture, tas_vsync. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tas.h"

uint64_t kernel_vsync_count;
static int online_stub;
static int pad_calls;          /* fields whose pad came from the movie */
static int window_closed;
static int delta_clock;        /* port_delta_clock(): the game steps by wall time when 1 */
static int rebase_calls;       /* port_clock_rebase(): the game clock's "no game time" resets */
static HostPad last_pad;       /* what the game consumed this field */

int pads_online(void) { return online_stub; }
int port_delta_clock(void) { return delta_clock; }
void port_clock_rebase(void) { rebase_calls++; }
void host_service_gs_pump_messages(void) { }
void host_service_gs_window_close(void) { window_closed = 1; }
double host_wall_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
void host_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
void host_end_add(int stage, const char *name, void (*fn)(void)) { (void)stage; (void)name; atexit(fn); }
void hle_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("LOG ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(3); }
    fputs(text, f);
    fclose(f);
}

/* One field, as the game runs it. */
static void field(void)
{
    HostPad in;
    tas_frame_advance_wait();                 /* kernel.c: the start of vsync() */
    kernel_vsync_count++;                     /* kernel.c: right after */
    memset(&in, 0, sizeof in);
    in.right_x = in.right_y = in.left_x = in.left_y = 128;
    in.buttons = (uint16_t)(PAD_CROSS | ((kernel_vsync_count & 1) ? PAD_L1 : 0));
    in.right_x = 200;                         /* "the controller" */
    in.right_y = 55;
    if (tas_playing() && tas_pad(0, &in)) pad_calls++;   /* pads_read -> host_pad_script */
    tas_capture(0, &in);                      /* pads_read's final input */
    last_pad = in;
    tas_vsync();                              /* kernel.c: the end of the field */
}

static void state(const char *tag)
{
    printf("%s playing=%d recording=%d paused=%d pad_calls=%d rebases=%d pad=%04X %u %u\n", tag,
           tas_playing(), tas_recording(), tas_paused(), pad_calls, rebase_calls, last_pad.buttons,
           last_pad.right_x, last_pad.right_y);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "";
    const char *step = getenv("DHD_TAS_STEP");
    int i;

    if (!strcmp(sc, "play3")) {
        for (i = 0; i < 3; i++) { field(); state("f"); }
        return 0;
    }
    if (!strcmp(sc, "rec3")) {
        for (i = 0; i < 3; i++) { field(); state("f"); }
        return 0;
    }
    if (!strcmp(sc, "online-play")) {
        online_stub = 1;
        field();
        state("online1");
        return 0;
    }
    if (!strcmp(sc, "online-rec")) {
        online_stub = 1;
        for (i = 0; i < 2; i++) { field(); state("online"); }
        return 0;
    }
    if (!strcmp(sc, "online-pause-env")) {
        online_stub = 1;
        field();                                  /* must not block */
        printf("FIELD_OK\n");
        state("after");
        return 0;
    }
    if (!strcmp(sc, "online-pause-cmd")) {
        online_stub = 1;
        write_file(step, "pause");
        field();                                  /* must not block */
        printf("FIELD_OK\n");
        state("after");
        return 0;
    }
    if (!strcmp(sc, "paused-then-online")) {
        write_file(step, "step");
        field();                                  /* one field, then paused again */
        state("paused");
        online_stub = 1;
        printf("ENTERING_ONLINE\n");
        fflush(stdout);
        field();                                  /* must not block: the gate drops the pause */
        printf("FIELD_OK\n");
        state("after");
        return 0;
    }
    if (!strcmp(sc, "online-leave")) {
        online_stub = 1;
        field();
        online_stub = 0;
        field();
        field();
        state("after_leave");
        printf("FIELDS %llu\n", (unsigned long long)kernel_vsync_count);
        return 0;
    }
    if (!strcmp(sc, "quit")) {
        write_file(step, "quit");
        tas_frame_advance_wait();                 /* returns for the field that ends the game */
        printf("after quit: paused=%d playing=%d window=%d\n", tas_paused(), tas_playing(), window_closed);
        return 0;
    }
    if (!strcmp(sc, "unknown")) {
        write_file(step, "wat");
        field();
        printf("AFTER_UNKNOWN\n");
        state("after");
        return 0;
    }
    if (!strcmp(sc, "nopause")) {
        field();
        field();
        state("after");
        return 0;
    }
    if (!strcmp(sc, "other-pads")) {
        /* a movie must not answer for pads 2-4, and must not record them */
        HostPad other;
        int taken;
        memset(&other, 0xAA, sizeof other);
        field();
        taken = tas_pad(1, &other);
        printf("other_pad_taken=%d untouched=%d\n", taken,
               other.buttons == 0xAAAA && other.right_x == 0xAA);
        state("after");
        return 0;
    }
    if (!strcmp(sc, "record-other")) {
        /* player 1's pad is recorded; a second pad's capture in the same field must not reach the file */
        HostPad mine, other;
        memset(&mine, 0, sizeof mine);
        mine.buttons = PAD_CROSS;
        mine.right_x = mine.right_y = mine.left_x = mine.left_y = 128;
        mine.right_x = 99;
        memset(&other, 0, sizeof other);
        other.buttons = PAD_SQUARE;
        other.right_x = 7;
        tas_frame_advance_wait();
        kernel_vsync_count++;
        tas_capture(0, &mine);
        tas_capture(1, &other);
        tas_vsync();
        state("after");
        return 0;
    }
    if (!strcmp(sc, "clock")) {
        delta_clock = atoi(getenv("STUB_DELTA_CLOCK") ? getenv("STUB_DELTA_CLOCK") : "0");
        field();
        state("after");
        return 0;
    }
    if (!strcmp(sc, "reject")) {
        field();                                  /* the integration path, not a helper */
        state("rejected");
        return 0;
    }
    fprintf(stderr, "unknown scenario %s\n", sc);
    return 2;
}
