/* A savestate for the TAS (finding 286, stage 1).

   Save the machine at a field boundary and load it back, in the same session, so that a run can be
   retried from a point instead of replayed from its start: the rerecording workflow. What is in a
   savestate is the *state trace's* state plus the two layers the trace deliberately omits (the EE
   threads' stacks, the HLE's own bookkeeping); what is knowingly missing is named in
   savestate_field()'s first log line, and finding 286 says why.

   Keys: Ctrl+5 saves, Ctrl+8 loads, in any session; both are off while an online race is (the TAS's
   own rule: it is offline only). Env: DHD_SAVE_STATE=<file> saves at DHD_SAVE_FIELD=<n>, or at the
   first boundary without one; DHD_LOAD_STATE=<file> loads at the first boundary. */
#ifndef DHD_SAVESTATE_H
#define DHD_SAVESTATE_H

#include <stdint.h>

/* kernel_init: reads the environment once. */
void savestate_init(void);
/* The field boundary, next to state_trace_field(): a pending load is applied, a pending save
   written, and the keys read. */
void savestate_field(void);

int savestate_save(const char *path);
int savestate_load(const char *path);

/* The file itself, over a block list: what savestate_save/load use, and what test_savestate.c can
   drive without a machine (the format, and every refusal). */
#define SS_NAME 24
typedef struct {
    char name[SS_NAME];
    void *data;        /* the bytes, when chunked is 0; the piece buffer when it is 1 */
    uint32_t bytes;
    int chunked;       /* 1: the GS prefix, copied a piece at a time through host_api.h */
} SsBlock;
int savestate_write_file(const char *path, const SsBlock *blocks, int count, uint32_t field);
int savestate_read_file(const char *path, const SsBlock *blocks, int count, uint32_t *field);

/* The HLE's own state, each unit's own: implemented where it lives (kernel.c, pad.c), sized at
   run time so a savestate unit never has to know another unit's structs. */
uint32_t kernel_state_size(void);
void kernel_state_save(void *dst);
void kernel_state_load(const void *src);
uint32_t pad_state_size(void);
void pad_state_save(void *dst);
void pad_state_load(const void *src);

#endif
