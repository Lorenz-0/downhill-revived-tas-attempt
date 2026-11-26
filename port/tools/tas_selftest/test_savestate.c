/* The savestate's file format (finding 286, savestate.c): what a file carries back, and what is
   refused whole before one byte of the machine is touched. No machine is needed: the blocks are
   three small buffers, and the reader is the same one savestate_load uses.

   The rest of the unit is stubbed, as the harness does for the TAS: the point here is the file, not
   the state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"
#include "hle.h"
#include "ps2asm.h"
#include "../gs/gs.h"

/* ---- the machine the unit expects, stubbed ---- */
extern ps2_cpu ps2;
ps2_cpu ps2;
uint64_t kernel_vsync_count;
uint64_t host64_guest_end = 0x80000000ull;
uint8_t *host64_high_base;
uintptr_t guest_base;              /* guest.h: the host address of guest address 0 */
void host64_memory_wild(uint64_t address) { (void)address; abort(); }
static void log_stub(const char *fmt, ...) { (void)fmt; }
void hle_log(const char *fmt, ...)
{
    va_list ap;   /* the refusals' lines: counted by the runner, not printed */
    va_start(ap, fmt);
    fputs("LOG ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}
const char *host_getenv(const char *name) { (void)name; return NULL; }
int host_service_keyboard_focused(void) { return 0; }
int host_service_keyboard_key(unsigned vk) { (void)vk; return 0; }
void host_service_gs_snapshot(uint32_t address, uint32_t bytes) { (void)address; (void)bytes; }
void host_service_graphics_wait(void) {}
void host_service_gs_restore(uint32_t address, uint32_t bytes) { (void)address; (void)bytes; }
void port_clock_rebase(void) {}
int pads_online(void) { return 0; }
int tas_playing(void) { return 0; }
int tas_recording(void) { return 0; }
uint32_t kernel_state_size(void) { return 8; }
void kernel_state_save(void *dst) { memset(dst, 0x11, 8); }
void kernel_state_load(const void *src) { (void)src; }
uint32_t pad_state_size(void) { return 4; }
void pad_state_save(void *dst) { memset(dst, 0x22, 4); }
void pad_state_load(const void *src) { (void)src; }
static void unused_log(void) { (void)log_stub; }

static void fill(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}

static int same(const void *a, const void *b, size_t n) { return memcmp(a, b, n) == 0; }

int main(void)
{
    const char *path = "savestate-selftest.state";
    const char *bad = "savestate-selftest-bad.state";
    uint8_t a[64], b[32], c[16], a2[64], b2[32], c2[16];
    SsBlock out[3] = {{"ram-ish", a, sizeof a}, {"gs-ish", b, sizeof b}, {"pad-ish", c, sizeof c}};
    SsBlock in[3] = {{"ram-ish", a2, sizeof a2}, {"gs-ish", b2, sizeof b2}, {"pad-ish", c2, sizeof c2}};
    uint32_t field = 0;
    int wrote, read, equal, field_ok;
    int refused_size, refused_name, refused_short, refused_magic;

    unused_log();
    fill(a, sizeof a, 3), fill(b, sizeof b, 91), fill(c, sizeof c, 200);
    memset(a2, 0, sizeof a2), memset(b2, 0, sizeof b2), memset(c2, 0, sizeof c2);

    wrote = savestate_write_file(path, out, 3, 4242);
    read = savestate_read_file(path, in, 3, &field);
    equal = same(a, a2, sizeof a) && same(b, b2, sizeof b) && same(c, c2, sizeof c);
    field_ok = field == 4242;
    printf("roundtrip wrote=%d read=%d equal=%d field=%u field-ok=%d\n", wrote, read, equal,
           (unsigned)field, field_ok);

    /* a block of another size: refused, and the targets are untouched (they were filled by the
       reader only after every name and size matched) */
    memset(a2, 0, sizeof a2);
    {
        SsBlock wrong[3] = {{"ram-ish", a2, sizeof a2 - 1}, {"gs-ish", b2, sizeof b2}, {"pad-ish", c2, sizeof c2}};
        refused_size = savestate_read_file(path, wrong, 3, NULL) == 0 && a2[0] == 0;
    }
    {
        SsBlock wrong[3] = {{"ram-ish", a2, sizeof a2}, {"gs-ISH", b2, sizeof b2}, {"pad-ish", c2, sizeof c2}};
        refused_name = savestate_read_file(path, wrong, 3, NULL) == 0 && a2[0] == 0;
    }
    /* a file that ends inside a block: refused */
    {
        FILE *f = fopen(path, "rb");
        static char whole[4096];
        size_t n = f ? fread(whole, 1, sizeof whole, f) : 0;
        if (f) fclose(f);
        f = fopen(bad, "wb");
        if (f) { fwrite(whole, 1, n > 40 ? n - 40 : n, f); fclose(f); }
        memset(a2, 0, sizeof a2);
        refused_short = savestate_read_file(bad, in, 3, NULL) == 0 && a2[0] == 0;
    }
    /* a file that is not one of ours */
    {
        FILE *f = fopen(bad, "wb");
        if (f) { fputs("this is not a savestate at all, not even close", f); fclose(f); }
        memset(a2, 0, sizeof a2);
        refused_magic = savestate_read_file(bad, in, 3, NULL) == 0 && a2[0] == 0;
    }
    printf("refusals size=%d name=%d short=%d magic=%d\n",
           refused_size, refused_name, refused_short, refused_magic);
    fflush(stdout);
    remove(path);
    remove(bad);
    return 0;
}
