/* Fault injection for the allocation-failure test: the movie's first chunk (1024 frames) is
   refused, and nothing else in the program is affected. */
#include <stddef.h>
#include <stdlib.h>

typedef struct { unsigned short buttons; unsigned char rx, ry, lx, ly; } TasPadShape;
typedef struct { unsigned long long field; TasPadShape pads[4]; int has; } TasFrameShape;

#define MOVIE_CHUNK (1024 * sizeof(TasFrameShape))

void *__real_realloc(void *p, size_t n);

void *__wrap_realloc(void *p, size_t n)
{
    if (n == MOVIE_CHUNK) return NULL;
    return __real_realloc(p, n);
}
