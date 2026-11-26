/* Integration-path tests for port/hle/tas.c: every field goes through the same order kernel.c
   and pads_read use — tas_frame_advance_wait(), ++kernel_vsync_count, the pad source
   (tas_pad when a movie plays, else "the controller"), tas_capture, tas_vsync. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "tas.h"
#include "controls_table.h"

/* Which copy of keyboard.c this build has (tas_selftest.py builds it two ways). Plain: the one
   copy, as a lone side. DHD_TAS_BRIDGE: only the host's, under its own names, so the core has no
   keyboard of its own to reach — which is the build that proves the TAS's keys cross the boundary
   (finding 285, revision 7). */
#ifdef DHD_TAS_BRIDGE
/* keyboard.c built with every public name prefixed host_ (tas_selftest.py) */
void host_keyboard_key(unsigned position, unsigned typed, int down);
void host_keyboard_focus(int focused);
void host_keyboard_pad(const struct ControlsLayout *layout, const uint8_t down[256], int riding,
                       double now, HostPad *out);
int host_keyboard_focused(void);
int host_keyboard_position_down(unsigned position);
#define KB_key host_keyboard_key
#define KB_focus host_keyboard_focus
#define KB_pad host_keyboard_pad
#define KB_focused host_keyboard_focused
#define KB_position_down host_keyboard_position_down
#else
#define KB_key keyboard_key
#define KB_focus keyboard_focus
#define KB_pad keyboard_pad
#define KB_focused keyboard_focused
#define KB_position_down keyboard_position_down
#endif

uint64_t kernel_vsync_count;
/* The HLE window the TAS publishes its claim in (port/runtime/memory.h): guest memory this test
   maps itself onto a buffer, so GUEST(PS2_TAS_KEYS) — the address the game's keyboard reads —
   lands in hle_window. guest.h's guest_base is the host address of guest address 0. */
static uint8_t hle_window[PS2_HLE_WINDOW_SIZE];
uintptr_t guest_base;
static int online_stub;
static int pad_calls;          /* fields whose pad came from the movie */
static int window_closed;
static int delta_clock;        /* port_delta_clock(): the game steps by wall time when 1 */
static int rebase_calls;       /* port_clock_rebase(): the game clock's "no game time" resets */
static HostPad last_pad;       /* what the game consumed this field */

int pads_online(void) { return online_stub; }
int port_delta_clock(void) { return delta_clock; }
void port_clock_rebase(void) { rebase_calls++; }
/* The TAS's keys go through keyboard.c itself (keyboard_key: what sdl_host.c calls for a real
   press), so the keys are read the way the game reads them, and the claim is applied where the
   game's pad is built. The paused loop calls host_service_gs_pump_messages once a pass, so that is
   where "the player" acts while the game is held: the press that frees it. */
static int pump_calls;
static int press_p_at;          /* the pump pass that presses P (0: never) */
static void press(unsigned place, int down) { KB_key(place, place, down); }
/* A player's hand seen while the game is held: the paused loop polls the keys every pass, so a
   release and the next press have to land on different passes to be two presses, as a hand's do. */
static struct { int at, place, down; } pump_presses[8];
static int pump_press_count;
static void press_at(int passes, unsigned place, int down)
{
    pump_presses[pump_press_count].at = pump_calls + passes;
    pump_presses[pump_press_count].place = (int)place;
    pump_presses[pump_press_count].down = down;
    pump_press_count++;
}
void host_service_gs_pump_messages(void)
{
    pump_calls++;
    if (press_p_at && pump_calls >= press_p_at) { press('P', 1); press_p_at = 0; }
    for (int i = 0; i < pump_press_count; i++)
        if (pump_presses[i].at && pump_calls >= pump_presses[i].at) {
            press((unsigned)pump_presses[i].place, pump_presses[i].down);
            pump_presses[i].at = 0;
        }
}
double host_pacing_period(void) { return 1.0 / 60; }   /* keyboard.c's field timing (host.c's) */

/* The keys as the core reads them (host_api.h, host.c): this stub host answers from the copy of
   keyboard.c the test presses keys into, exactly as the real host answers from its own. The real
   crossing — a host copy SDL writes and a module copy it never does — is
   port/tools/tas_selftest/test_tas_bridge.c. */
static int bridge_reads;
int host_service_keyboard_focused(void) { bridge_reads++; return KB_focused(); }
int host_service_keyboard_key(unsigned vk) { bridge_reads++; return KB_position_down(vk); }
void host_service_gs_window_close(void) { window_closed = 1; }
/* The stub host's clock, with the port's contract (host_api.h, hle.h): host_wall_seconds is a
   monotonic wall clock in seconds, and host_sleep_ms really blocks. Both matter to the unit under
   test: tas.c's pause times a 4 ms sleep to decide whether this host can hold the game at all
   (host_can_block), so a sleep that returned at once would silently turn frame advance off.
   Each platform uses its own timing API; the tests are the same on both. */
#ifdef _WIN32
double host_wall_seconds(void)
{
    static LARGE_INTEGER frequency;   /* counted once; the tests are single-threaded */
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    if (!frequency.QuadPart) return 0.0;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}
void host_sleep_ms(unsigned ms) { Sleep((DWORD)ms); }
#else
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
#endif
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

/* The key scenario's line: the field count the action left, and whether the game is held. Named
   apart from state() so the checks that read a state() line stay one line each. */
static void note(const char *tag)
{
    printf("%s fields=%llu paused=%d\n", tag, (unsigned long long)kernel_vsync_count, tas_paused());
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "";
    const char *step = getenv("DHD_TAS_STEP");
    int i;

    guest_base = (uintptr_t)hle_window - PS2_HLE_WINDOW;   /* the guest's window is this buffer */
    KB_focus(1);                                           /* the game's window has the keyboard */

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
    if (!strcmp(sc, "keys-pad")) {
        /* the claim where the game feels it: a key a TAS session owns reaches no pad button, and
           an ordinary key still does. keyboard.c is the file that turns a layout's keys into a pad
           (the host's copy is the one player 1's pad comes from). */
        HostPad p;
        uint8_t down[256];
        const volatile uint8_t *words;
        if (!strcmp(sc, "keys-pad") && getenv("STUB_ONLINE_FIELD")) online_stub = 1;
        tas_init();
        if (online_stub) field();                 /* the online gate ends the session's claim */
        words = tas_claim_words();
        printf("words=%02X%02X%02X%02X\n", words[4], words[6], words[8], words[10]);
        memset(down, 0, sizeof down);
        down['C'] = down[' '] = down['A'] = 1;    /* kick (Circle), pedal (Cross), L1 */
        memset(&p, 0, sizeof p);
        p.left_x = p.left_y = p.right_x = p.right_y = 128;
        KB_pad(&control_defaults[CONTROL_KEYBOARD], down, 1, 1.0, &p);
        printf("pad=%04X cross=%d circle=%d l1=%d\n", p.buttons, (p.buttons & PAD_CROSS) != 0,
               (p.buttons & PAD_CIRCLE) != 0, (p.buttons & PAD_L1) != 0);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(sc, "claim")) {
        /* which keys the TAS owns this run: the game's layout never sees these (host.c) */
        tas_init();                               /* as the first field does, before any pad read */
        printf("words=%02X%02X%02X%02X\n", tas_claim_words()[4], tas_claim_words()[6],
               tas_claim_words()[8], tas_claim_words()[10]);
        printf("claimed C=%d space=%d one=%d P=%d other=%d\n", tas_key_claimed('C'),
               tas_key_claimed(0x20), tas_key_claimed('1'), tas_key_claimed('P'), tas_key_claimed('X'));
        return 0;
    }
    if (!strcmp(sc, "bridge")) {
        /* The keys reach the TAS from the host's keyboard and from nothing else: P holds the game,
           C then moves exactly one field and it stays held. In the DHD_TAS_BRIDGE build the only
           keyboard in the program is the host's (under its own names), so a core that read its own
           copy would not even link. */
        int reads, held;
        unsigned long long before;
        press('P', 1);
        field();                                  /* P down: the game is held from here */
        reads = bridge_reads, held = tas_paused();
        press('P', 0);
        press('C', 1);
        before = (unsigned long long)kernel_vsync_count;
        field();                                  /* C down: exactly one field, still held */
        printf("bridge reads=%d paused=%d after-c paused=%d fields=%llu\n", reads, held, tas_paused(),
               (unsigned long long)kernel_vsync_count - before);
        fflush(stdout);
        return 0;                                 /* ends while held: nothing outlives it */
    }
    if (!strcmp(sc, "keys")) {
        /* the TAS's keys with no control file at all: P holds the game and releases it, C, Space
           and 1 step one field each, and a key held down steps nothing more. Each press is polled
           by the field it starts (a level, so a key that never goes up is never new again). */
        int before;
        tas_init();
        printf("claimed C=%d space=%d one=%d P=%d other=%d\n", tas_key_claimed('C'),
               tas_key_claimed(0x20), tas_key_claimed('1'), tas_key_claimed('P'), tas_key_claimed('X'));
        press('P', 1);                        /* running: P pauses before the next field */
        field();
        state("press-p");
        note("k-press-p");
        press('P', 0);
        press('C', 1);                        /* paused: C releases exactly one field */
        field();
        state("step-c");
        note("k-step-c");
        /* C is still down. Nothing may release the field until the pump presses P, 31 passes on. */
        press_p_at = pump_calls + 31;
        before = pump_calls;
        field();
        printf("held_pumps=%d\n", pump_calls - before);
        fflush(stdout);
        state("after-held");
        note("k-after-held");
        press('C', 0);                        /* every key up, one field to let the levels go */
        press('P', 0);
        field();
        state("settled");
        note("k-settled");
        press('P', 1);                        /* running: P pauses */
        field();
        state("press-p-2");
        note("k-press-p-2");
        press('P', 0);
        press(0x20, 1);                       /* paused: Space steps one field */
        field();
        state("step-space");
        note("k-step-space");
        press(0x20, 0);
        press('1', 1);                        /* paused: 1 steps one field */
        field();
        state("step-one");
        note("k-step-one");
        press('1', 0);
        press('P', 1);                        /* paused: P releases the game */
        field();
        state("end");
        note("k-end");
        return 0;
    }
    if (!strcmp(sc, "step-run")) {
        /* a step key from a *running* game: the field it is sitting on runs, then the game is held
           (the first PC run pressed C, Space and 1 while the game was running and concluded the
           keys did nothing: a step used to need P first, and nothing said so). */
        unsigned long long before;
        tas_init();
        press('C', 1);
        before = (unsigned long long)kernel_vsync_count;
        field();                                  /* running: C holds at this boundary */
        printf("step-run paused=%d fields=%llu\n", tas_paused(),
               (unsigned long long)kernel_vsync_count - before);
        /* the release and the next press land while the game is held: a hand moves faster than the
           loop, but not between two looks at it */
        press_at(1, 'C', 0);
        press_at(3, 'C', 1);                      /* held: the next press releases one field */
        before = (unsigned long long)kernel_vsync_count;
        field();
        printf("step-run press=%d fields=%llu\n", tas_paused(),
               (unsigned long long)kernel_vsync_count - before);
        fflush(stdout);
        return 0;                                 /* ends held: nothing outlives it */
    }
    if (!strcmp(sc, "clock-contract")) {
        /* the timing shim itself: monotonic, and a sleep that really sleeps */
        double before = host_wall_seconds();
        host_sleep_ms(20);
        double after = host_wall_seconds();
        double again = host_wall_seconds();
        printf("monotonic=%d slept_ms=%.1f\n", again >= after && after >= before, (after - before) * 1000.0);
        fflush(stdout);
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
