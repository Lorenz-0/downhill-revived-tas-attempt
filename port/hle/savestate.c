/* A savestate for the TAS (finding 286, stage 1). See savestate.h.

   The blocks are the state trace's own list where it can be reached — RAM, the EE thread stacks,
   the scratchpad, the HLE window, VU0/VU1's memory, the CPU's registers, the whole GS prefix —
   plus the two layers the trace leaves out because they are host state: the kernel's threads,
   semaphores, alarms and counters, and padman's frames. The renderer, the sound device, the disc's
   open files and the graphics queue are not state in this sense (finding 286: they are re-creatable
   or re-openable) and the first cut leaves them alone deliberately.

   A savestate belongs to the session and the build that wrote it: the code's live frames are host
   memory that a fresh boot has not got (finding 286's one obstacle), so loading is supported within
   the session that saved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"
#include "hle.h"
#include "fps.h"
#include "ps2asm.h"
#include "../gs/gs.h"

extern ps2_cpu ps2;
extern uint64_t kernel_vsync_count;   /* kernel.c: the field a savestate belongs to */

/* The format. A block is a name and its bytes, in a fixed order: the reader checks every name and
   size before it writes one byte, so a file that is not this state is refused whole. */
#define SS_MAGIC "DHDSTAT1"
enum { SS_VERSION = 1, SS_MAX_BLOCKS = 12 };
typedef struct {
    char magic[8];
    uint32_t version, field, blocks, total;
} SsHeader;

/* The images of the state that is not in guest memory: the GS prefix (4 MiB, gs.h) and the
   kernel's and padman's own state, each sized by its owner at the first use and kept from then on.

   They are the heap's, not the module's data, for a measured reason: the core's data image already
   runs from 0x70200000 to about 0x70D9C000 inside an initial memory that ends at 0x70E00000
   (build_wasm.py), so a 4 MiB array here is "initial memory too small" at the wasm link — the first
   cut of this unit failed to build for exactly that. state_trace.c hands a malloc'd packet to a host
   service the same way (GADDR(packet), host_service_gs_trace_pop), so a heap buffer is a buffer the
   host services can be given in both builds. */
static void *gs_image;   /* offsetof(Gs, sync) bytes: the GS prefix */
static uint8_t *kernel_image, *pad_image;
static uint32_t kernel_bytes, pad_bytes;

static int images(void)
{
    if (kernel_image) return 1;
    kernel_bytes = kernel_state_size();
    pad_bytes = pad_state_size();
    gs_image = malloc(offsetof(Gs, sync));
    kernel_image = malloc(kernel_bytes ? kernel_bytes : 1);
    pad_image = malloc(pad_bytes ? pad_bytes : 1);
    if (!gs_image || !kernel_image || !pad_image) {
        free(gs_image), free(kernel_image), free(pad_image);
        gs_image = NULL, kernel_image = NULL, pad_image = NULL;
        hle_log("savestate: no memory for the state's images (%.1f MB) — savestates are off\n",
                (double)(offsetof(Gs, sync) + kernel_bytes + pad_bytes) / (1024.0 * 1024.0));
        return 0;
    }
    /* Touched now, so that memory this unit cannot have fails here with a line, not later inside a
       copy of the machine. */
    memset(gs_image, 0, offsetof(Gs, sync));
    memset(kernel_image, 0, kernel_bytes);
    memset(pad_image, 0, pad_bytes);
    hle_log("savestate: images %.1f MB (gs %.1f, kernel %.1f, pad %.1f)\n",
            (double)(offsetof(Gs, sync) + kernel_bytes + pad_bytes) / (1024.0 * 1024.0),
            (double)offsetof(Gs, sync) / (1024.0 * 1024.0), (double)kernel_bytes / (1024.0 * 1024.0),
            (double)pad_bytes / (1024.0 * 1024.0));
    return 1;
}

static char save_path[512], load_path[512];
static char quick_path[512];
static int save_pending, load_pending, key_last;
static unsigned long save_field;
static int said_lines, said_blocks;

/* The blocks of the first save and the first load, in dhd.log: if one of them is where a run dies,
   the last line says which (the later saves stay quiet). */
static void log_block(const char *what, const char *name, uint32_t bytes)
{
    if (said_blocks) return;
    hle_log("savestate: %s %-7s %u bytes\n", what, name, (unsigned)bytes);
}

static int session_live(void)
{
    /* A savestate's keys are the TAS session's keys: a movie, a control file, or the keys asked for,
       so an ordinary player's F5 and F8 stay the game's. */
    static const char *const names[] = {"DHD_TAS_PLAY", "DHD_TAS_RECORD", "DHD_TAS_STEP",
                                        "DHD_TAS_KEYS", "DHD_TAS_PAUSE", "DHD_SAVE_STATE",
                                        "DHD_LOAD_STATE"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        const char *v = host_getenv(names[i]);
        if (v && *v && strcmp(v, "0") != 0) return 1;
    }
    return 0;
}

int savestate_enabled(void) { return save_path[0] || load_path[0] || session_live(); }

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
    if (!save_path[0] && !load_path[0] && session_live())
        hle_log("savestate: F5 saves to '%s' and F8 loads it back (a TAS session's keys)\n", quick_path);
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
    if (!images()) return 0;
    kernel_state_save(kernel_image);
    pad_state_save(pad_image);
    /* Every packet the field fed has reached the GS state (state_trace.c waits the same way before
       its own snapshot). */
    host_service_graphics_wait();
    host_service_gs_snapshot(GADDR(gs_image), (uint32_t)offsetof(Gs, sync));
    add(b, &n, "ram", GUEST(0), 0x02000000u);
    /* Not the 64 EE thread stacks at 0x06000000 (16 MiB): the host commits those page by page, per
       slot, and copying the whole area reads pages that were never committed — which ends the
       process with the guest's own fault, as the first save did on Windows. They are the same class
       of state as the live frames (finding 286) and are not in a savestate yet. */
    add(b, &n, "scratch", GUEST(0x70000000), 0x4000u);
    add(b, &n, "hlew", GUEST(PS2_HLE_WINDOW), PS2_HLE_WINDOW_SIZE);
    add(b, &n, "vu", GUEST(0x11000000), 0x10000u);
    add(b, &n, "cpu", &ps2, (uint32_t)sizeof ps2);
    add(b, &n, "gs", gs_image, (uint32_t)offsetof(Gs, sync));
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
    /* The table of contents, then the bytes: the reader checks every name and size before it writes
       one of them (a file that is not this state must leave the machine whole). */
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (int i = 0; i < count && ok; i++) {
        char name[SS_NAME] = {0};
        uint32_t bytes = blocks[i].bytes;
        memcpy(name, blocks[i].name, sizeof name);
        ok = fwrite(name, sizeof name, 1, f) == 1 && fwrite(&bytes, sizeof bytes, 1, f) == 1;
    }
    for (int i = 0; i < count && ok; i++) {
        log_block("write", blocks[i].name, blocks[i].bytes);
        ok = fwrite(blocks[i].data, 1, blocks[i].bytes, f) == blocks[i].bytes;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        hle_log("savestate: '%s' was not written whole\n", path);
        return 0;
    }
    return 1;
}

int savestate_save(const char *path)
{
    SsBlock b[SS_MAX_BLOCKS];
    int n = collect(b);
    if (!n) return 0;
    if (!savestate_write_file(path, b, n, (uint32_t)kernel_vsync_count)) return 0;
    said_blocks = 1;
    hle_log("savestate: saved '%s' at field %llu, %.1f MB (ram, scratch, the HLE window, VU, the CPU, "
            "the GS, the kernel's threads and padman; not the EE stacks or the live frames — finding "
            "286)\n", path, (unsigned long long)kernel_vsync_count, megabytes(b, n));
    return 1;
}

/* Everything is checked before anything is written: the header, the block count, then each block's
   name and size against the list collect() would make. Only then are the bytes read into place, so a
   file that is not this state leaves it whole. */
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
    /* Pass 1: the table of contents against the list, and nothing else — a file that is not this
       state must leave the machine as it was, so no block is written until every one has matched. */
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
    /* The file's own length: a truncated file is refused here, still without having written a byte. */
    long data_start = (long)sizeof h + (long)count * (long)(SS_NAME + sizeof(uint32_t));
    long want = data_start;
    for (int i = 0; i < count; i++) want += (long)blocks[i].bytes;
    long got = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (got != want) {
        fclose(f);
        hle_log("savestate: '%s' is %ld bytes, this state is %ld: refused whole\n", path, got, want);
        return 0;
    }
    /* Pass 2: the bytes, from where they start. */
    if (fseek(f, data_start, SEEK_SET) != 0) {
        fclose(f);
        hle_log("savestate: '%s' cannot be read from its first block\n", path);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        log_block("read", blocks[i].name, blocks[i].bytes);
        if (fread(blocks[i].data, 1, blocks[i].bytes, f) != blocks[i].bytes) {
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
    int n = collect(b);
    uint32_t field = 0;
    if (!n) return 0;
    if (!savestate_read_file(path, b, n, &field)) return 0;
    /* The blocks are in place. What the units hold outside guest memory goes back, then the clock is
       rebased: the time between the save and the load is no game time. */
    host_service_gs_restore(GADDR(gs_image), (uint32_t)offsetof(Gs, sync));
    kernel_state_load(kernel_image);
    pad_state_load(pad_image);
    port_clock_rebase();
    said_blocks = 1;
    hle_log("savestate: loaded '%s' at field %llu (saved at %u); the renderer's own caches are not "
            "state: a picture may be stale until the game redraws\n", path,
            (unsigned long long)kernel_vsync_count, (unsigned)field);
    return 1;
}

void savestate_field(void)
{
    int f5 = 0, f8 = 0;
    if (!savestate_enabled()) return;
    if (load_pending) {
        load_pending = 0;
        savestate_load(load_path);
    }
    if (save_pending && (!save_field || kernel_vsync_count >= save_field)) {
        save_pending = 0;
        savestate_save(save_path);
    }
    if (!session_live()) return;
    /* The keys: F5 and F8, on their edges, from the host's keyboard (the same two calls the TAS's
       own keys come through, so no second import). */
    if (host_service_keyboard_focused()) {
        f5 = host_service_keyboard_key(0x74);
        f8 = host_service_keyboard_key(0x77);
    }
    int edge5 = f5 && !(key_last & 1), edge8 = f8 && !(key_last & 2);
    key_last = (f5 ? 1 : 0) | (f8 ? 2 : 0);
    if (edge5) {
        snprintf(quick_path, sizeof quick_path, "dhd-quicksave.state");
        savestate_save(quick_path);
    }
    if (edge8) savestate_load(quick_path);
}
