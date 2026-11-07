#include "tiny_b.h"
#include <tiny_a.h>

int tiny_b(void) { return tiny_a() + 1 + TINY_BONUS; }

#ifdef __APPLE__
#include <mach/mach_time.h>

// A required-reason API, and tiny-b has no privacy manifest: installing it for Apple warns.
unsigned long long tiny_b_ticks(void);
unsigned long long tiny_b_ticks(void) { return mach_absolute_time(); }
#endif
