/* Fault injection: the third flush that is not stdout/stderr fails — the header, field 1, then
   the write of field 2. */
#include <stdio.h>

int __real_fflush(FILE *f);

int __wrap_fflush(FILE *f)
{
    static int n;
    if (f == stdout || f == stderr) return __real_fflush(f);
    if (++n == 3) return EOF;
    return __real_fflush(f);
}
