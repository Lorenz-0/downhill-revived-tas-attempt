/* A savestate for the TAS (finding 286, stage 1). See savestate.h.

   The blocks are the state trace's own list where it can be reached — RAM, the scratchpad, the HLE
   window, VU0/VU1's memory, the CPU's registers — plus the GS prefix (streamed, below) and the two
   layers the trace leaves out because they are host state: the kernel's threads, semaphores, alarms
   and counters, and padman's frames. The renderer, the sound device, the disc's open files and the
   graphics queue are not state in this sense (finding 286: they are re-creatable or re-openable).

   Two things this unit has learned the hard way, both of them about where a copy may read and write:

   - The 64 EE thread stacks at 0x06000000 are committed page by page, per slot, with a guard page
     each: copying the whole 16 MiB area reads pages that were never committed and the process ends
     as the guest's own fault does. They are not in a savestate (finding 286).
   - A 4 MiB array here does not fit the module's data — the wasm link refuses it ("initial memory
     too small"). The GS prefix was streamed through a 64 KiB piece at a time instead, behind a wait
     for the graphics workers, and that wait is what this unit no longer does: the port draws on
     worker threads and defers the vsync (finding 090 HO1), so waiting for the drawing from inside
     the field stalls the pipeline the next field needs. A save in the main menu worked and a save
     in a race never returned (finding 286). The GS is not in a savestate now, which is what this
     unit's own rule says happened to the renderer's caches: they are re-creatable, not state. It
     comes back when there is a point in the field where the workers are quiet by construction.

   A savestate belongs to the session and the build that wrote it: the code's live frames are host
   memory that a fresh boot has not got (finding 286's one obstacle), so loading is supported within
   the session that saved.

   The keys save and load in any session, movie or not (a player practising a track has no movie
   either), and are off while an online race is: that is the TAS's rule and this is the TAS's state.
   The first log line says the keys are there, so a report's log says whether the build has them. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"
#include "hle.h"
#include "tas.h"
#include "fps.h"
#include "ps2asm.h"
#include "../runtime/memory.h"   /* PS2_RAM_BASE: where PS2 address 0 is in the module */

extern ps2_cpu ps2;
extern uint64_t kernel_vsync_count;   /* kernel.c: the field a savestate belongs to */

/* A breadcrumb beside the log: dhd.log is a whole session and a report may arrive with another
   run's (two did), so every action here also appends one line to savestate-trace.txt, opened and
   closed per line so that the last line is on disk before anything can end the process. The last
   line a run wrote is where it died, or how far it got before the key was ever read.
   Not in the game's log except first-time block lines: at 165 fields/s a save's eight lines per
   press belong in this file, not in dhd.log. */
#define SS_TRACE "savestate-trace.txt"
static void breadcrumb(const char *fmt, ...)
{
    va_list ap;
    FILE *f = fopen(SS_TRACE, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The format. A block is a name and its bytes, in a fixed order: the reader checks every name and
   size, and the file's own length, before it writes one byte of the machine. */
#define SS_MAGIC "DHDSTAT1"
enum {
    SS_VERSION = 1,
    SS_MAX_BLOCKS = 12,
    KERNEL_IMAGE_MAX = 256 * 1024,     /* kernel.c's KernelState is about 205 KiB */
    PAD_IMAGE_MAX = 4096
};
typedef struct {
    char magic[8];
    uint32_t version, field, blocks, total;
} SsHeader;

/* The two units' own state. Static: no allocation, and nothing here is bigger than 256 KiB. */
static uint8_t kernel_image[KERNEL_IMAGE_MAX], pad_image[PAD_IMAGE_MAX];
static uint32_t kernel_bytes, pad_bytes;

static char save_path[512], load_path[512];
static char quick_path[512];
static int save_pending, load_pending, key_last;
static unsigned long save_field;
static int said_lines, said_blocks;

/* An online race owns the simulation: the TAS turns itself off there, and so does this (finding 286,
   as mission 7's scope says). Said once. */
static int online_said;
static int online_blocks(void)
{
    if (!pads_online()) return 0;
    if (!online_said) {
        online_said = 1;
        hle_log("savestate: Ctrl+5 and Ctrl+8 off: an online session (a savestate is the TAS's, and an "
                "online race is not\n");
        breadcrumb("keys: off, an online session");
    }
    return 1;
}

static int sizes(void)
{
    kernel_bytes = kernel_state_size();
    pad_bytes = pad_state_size();
    if (kernel_bytes > sizeof kernel_image || pad_bytes > sizeof pad_image) {
        hle_log("savestate: a unit's state is larger than this one's image (kernel %u of %u, pad %u of "
                "%u) — savestates are off\n", (unsigned)kernel_bytes, (unsigned)sizeof kernel_image,
                (unsigned)pad_bytes, (unsigned)sizeof pad_image);
        return 0;
    }
    return 1;
}

void savestate_init(void)
{
    const char *p;
    if (said_lines) return;
    said_lines = 1;
    if ((p = host_getenv("DHD_SAVE_STATE")) && *p) snprintf(save_path, sizeof save_path, "%s", p);
    if ((p = host_getenv("DHD_LOAD_STATE")) && *p) snprintf(load_path, sizeof load_path, "%s", p);
    if ((p = host_getenv("DHD_SAVE_FIELD")) && *p) save_field = (unsigned long)strtoul(p, NULL, 0);
    snprintf(quick_path, sizeof quick_path, "%s", "dhd-quicksave.state");
    if (save_path[0]) save_pending = 1;
    if (load_path[0]) load_pending = 1;
    if (save_path[0]) hle_log("savestate: save to '%s' at %s (DHD_SAVE_STATE)\n", save_path,
                              save_field ? "its field" : "the first field");
    if (load_path[0]) hle_log("savestate: load '%s' at the first field (DHD_LOAD_STATE; a file from a "
                              "fresh boot cannot bring the code's live frames back — finding 286)\n", load_path);
    breadcrumb("boot: savestates on; Ctrl+5 saves to %s, Ctrl+8 loads it back", quick_path);
    /* Always said: the log then shows which build a report came from, and the keys' names. */
    hle_log("savestate: Ctrl+5 saves to '%s' and Ctrl+8 loads it back, in any session (both are off "
            "while an online race is; a savestate is the TAS's, finding 286)\n", quick_path);
}

static void add(SsBlock *b, int *n, const char *name, void *data, uint32_t bytes)
{
    if (*n >= SS_MAX_BLOCKS) return;
    snprintf(b[*n].name, SS_NAME, "%s", name);
    b[*n].data = data;
    b[*n].bytes = bytes;
    (*n)++;
}

/* The state, in a fixed order: the same list the reader expects. */
static int collect(SsBlock *b)
{
    int n = 0;
    if (!sizes()) return 0;
    kernel_state_save(kernel_image);
    pad_state_save(pad_image);
    /* PS2 address 0 is at PS2_RAM_BASE (0x08000000) inside the module, not at 0: the module's own
       shadow stack and its null guard page live in the first 4 MiB of its memory, so a copy from 0
       reads the stack and then the guard page, and the runtime ends the process with an
       out-of-bounds trap. This unit did exactly that on Windows (finding 286): the log's last line
       was "savestate: write ram 33554432 bytes" and then "host64 trap: Out-of-bounds access". */
    add(b, &n, "ram", (void *)(uintptr_t)PS2_RAM_BASE, 0x02000000u);
    /* Not the 64 EE thread stacks at 0x06000000: see the note at the top of this file. */
    add(b, &n, "scratch", GUEST(PS2_SCRATCHPAD), 0x4000u);
    add(b, &n, "hlew", GUEST(PS2_HLE_WINDOW), PS2_HLE_WINDOW_SIZE);
    add(b, &n, "vu", GUEST(0x11000000), 0x10000u);   /* as state_trace.c reads it (finding 286) */
    add(b, &n, "cpu", &ps2, (uint32_t)sizeof ps2);
    add(b, &n, "kernel", kernel_image, kernel_bytes);
    add(b, &n, "pad", pad_image, pad_bytes);
    return n;
}

static double megabytes(const SsBlock *b, int count)
{
    unsigned long long total = 0;
    for (int i = 0; i < count; i++) total += b[i].bytes;
    return (double)total / (1024.0 * 1024.0);
}

/* The blocks of the first save and the first load, in dhd.log: if one of them is where a run dies,
   the last line says which (the later ones stay quiet). */
static void log_block(const char *what, const char *name, uint32_t bytes)
{
    if (said_blocks) return;
    hle_log("savestate: %s %-7s %u bytes\n", what, name, (unsigned)bytes);
}

/* One block's bytes, with a breadcrumb before each: a run that ends inside one says which. */
static int put(FILE *f, const SsBlock *b)
{
    breadcrumb("save: block %s %u bytes", b->name, (unsigned)b->bytes);
    log_block("write", b->name, b->bytes);
    return fwrite(b->data, 1, b->bytes, f) == b->bytes;
}

static int get(FILE *f, const SsBlock *b)
{
    breadcrumb("load: block %s %u bytes", b->name, (unsigned)b->bytes);
    log_block("read", b->name, b->bytes);
    return fread(b->data, 1, b->bytes, f) == b->bytes;
}

int savestate_write_file(const char *path, const SsBlock *blocks, int count, uint32_t field)
{
    SsHeader h;
    FILE *f = fopen(path, "wb");
    if (!f) {
        hle_log("savestate: cannot write '%s'\n", path);
        return 0;
    }
    memset(&h, 0, sizeof h);
    memcpy(h.magic, SS_MAGIC, 8);
    h.version = SS_VERSION;
    h.field = field;
    h.blocks = (uint32_t)count;
    for (int i = 0; i < count; i++) h.total += blocks[i].bytes;
    /* The table of contents, then the bytes: the reader checks every name and size, and the file's
       length, before it writes one of them. */
    breadcrumb("save: file open, table next");
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (int i = 0; i < count && ok; i++) {
        char name[SS_NAME] = {0};
        uint32_t bytes = blocks[i].bytes;
        memcpy(name, blocks[i].name, sizeof name);
        ok = fwrite(name, sizeof name, 1, f) == 1 && fwrite(&bytes, sizeof bytes, 1, f) == 1;
    }
    breadcrumb("save: table written, blocks next");
    for (int i = 0; i < count && ok; i++) ok = put(f, &blocks[i]);
    if (fclose(f) != 0) ok = 0;
    breadcrumb("save: file closed");
    if (!ok) {
        hle_log("savestate: '%s' was not written whole\n", path);
        return 0;
    }
    return 1;
}

int savestate_save(const char *path)
{
    SsBlock b[SS_MAX_BLOCKS];
    breadcrumb("save: asked at field %llu (%s)", (unsigned long long)kernel_vsync_count, path);
    int n = collect(b);
    if (!n) {
        breadcrumb("save: refused, nothing collected");
        return 0;
    }
    breadcrumb("save: %d blocks, %.1f MB", n, megabytes(b, n));
    if (!savestate_write_file(path, b, n, (uint32_t)kernel_vsync_count)) {
        breadcrumb("save: the file was not written");
        return 0;
    }
    breadcrumb("save: done, %.1f MB", megabytes(b, n));
    said_blocks = 1;
    hle_log("savestate: saved '%s' at field %llu, %.1f MB (ram, scratch, the HLE window, VU, the CPU, "
            "the kernel's threads and padman; not the EE stacks, the live frames or the renderer's "
            "own state — finding 286)\n", path, (unsigned long long)kernel_vsync_count, megabytes(b, n));
    return 1;
}

/* Everything is checked before anything is written: the header, the block count, each block's name
   and size, and the file's length. Only then are the bytes read into place, so a file that is not
   this state leaves the machine whole. */
int savestate_read_file(const char *path, const SsBlock *blocks, int count, uint32_t *field)
{
    SsHeader h;
    FILE *f = fopen(path, "rb");
    if (!f) {
        hle_log("savestate: cannot read '%s'\n", path);
        return 0;
    }
    int ok = fread(&h, sizeof h, 1, f) == 1;
    if (!ok || memcmp(h.magic, SS_MAGIC, 8) != 0 || h.version != SS_VERSION || h.blocks != (uint32_t)count) {
        fclose(f);
        hle_log("savestate: '%s' is not a savestate of this build (magic, version or %d blocks)\n",
                path, count);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        char name[SS_NAME];
        uint32_t bytes = 0;
        if (fread(name, sizeof name, 1, f) != 1 || fread(&bytes, sizeof bytes, 1, f) != 1
            || bytes != blocks[i].bytes || memcmp(name, blocks[i].name, SS_NAME) != 0) {
            fclose(f);
            hle_log("savestate: '%s' does not match this state at block %d ('%s')\n", path, i,
                    blocks[i].name);
            return 0;
        }
    }
    long data_start = (long)sizeof h + (long)count * (long)(SS_NAME + sizeof(uint32_t));
    long want = data_start;
    for (int i = 0; i < count; i++) want += (long)blocks[i].bytes;
    long got = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (got != want) {
        fclose(f);
        hle_log("savestate: '%s' is %ld bytes, this state is %ld: refused whole\n", path, got, want);
        return 0;
    }
    if (fseek(f, data_start, SEEK_SET) != 0) {
        fclose(f);
        hle_log("savestate: '%s' cannot be read from its first block\n", path);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!get(f, &blocks[i])) {
            fclose(f);
            hle_log("savestate: '%s' ended inside block %d ('%s')\n", path, i, blocks[i].name);
            return 0;
        }
    }
    fclose(f);
    if (field) *field = h.field;
    return 1;
}

int savestate_load(const char *path)
{
    SsBlock b[SS_MAX_BLOCKS];
    breadcrumb("load: asked at field %llu (%s)", (unsigned long long)kernel_vsync_count, path);
    int n = collect(b);
    uint32_t field = 0;
    if (!n) {
        breadcrumb("load: refused, nothing collected");
        return 0;
    }
    if (!savestate_read_file(path, b, n, &field)) {
        breadcrumb("load: the file was refused or ended early");
        return 0;
    }
    breadcrumb("load: %d blocks back, saved at field %u", n, (unsigned)field);
    /* What the units hold outside guest memory goes back now, then the clock is rebased: the time
       between the save and the load is no game time. The renderer keeps what it had (it is not in a
       savestate): a picture can be stale until the game draws again. */
    kernel_state_load(kernel_image);
    pad_state_load(pad_image);
    port_clock_rebase();
    said_blocks = 1;
    hle_log("savestate: loaded '%s' at field %llu (saved at %u); the renderer's own caches are not "
            "state: a picture may be stale until the game redraws\n", path,
            (unsigned long long)kernel_vsync_count, (unsigned)field);
    /* A movie's own position (its field count, its file offset) is not in a savestate: the machine
       went back, the movie did not, so the two are out of step from here. */
    if (tas_playing() || tas_recording())
        hle_log("savestate: a movie is %s, and its position is not in a savestate: the machine is back "
                "at field %u and the movie is not (stop the movie before loading, or the run will not "
                "replay)\n", tas_playing() ? "playing" : "recording", (unsigned)field);
    return 1;
}

void savestate_field(void)
{
    int ctrl, k5 = 0, k8 = 0;
    if (load_pending) {
        load_pending = 0;
        savestate_load(load_path);
    }
    if (save_pending && (!save_field || kernel_vsync_count >= save_field)) {
        save_pending = 0;
        savestate_save(save_path);
    }
    if (online_blocks()) return;
    /* The keys: Ctrl+5 saves and Ctrl+8 loads, on their edges, from the host's keyboard (the same two
       calls the TAS's own keys come through). Ctrl, so a plain 5 or 8 stays the game's. */
    ctrl = host_service_keyboard_focused() && host_service_keyboard_key(VKEY_CONTROL);
    if (ctrl) {
        k5 = host_service_keyboard_key(0x35);
        k8 = host_service_keyboard_key(0x38);
    }
    int edge5 = k5 && !(key_last & 1), edge8 = k8 && !(key_last & 2);
    key_last = (k5 ? 1 : 0) | (k8 ? 2 : 0);
    if (edge5 || edge8)
        breadcrumb("key: %s at field %llu", edge5 ? "Ctrl+5" : "Ctrl+8",
                   (unsigned long long)kernel_vsync_count);
    if (edge5) savestate_save(quick_path);
    if (edge8) savestate_load(quick_path);
}
