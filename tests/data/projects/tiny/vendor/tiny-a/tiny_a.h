int tiny_a(void);

// TINY_A_EXTRA comes from the product's defines (recipe-toml.md 11): on Apple, through the module
#ifdef TINY_A_EXTRA
static inline int tiny_a_extra(void) { return TINY_A_EXTRA; }
#endif

#ifdef __APPLE__
// needs libz, which the product links through its shim target
int tiny_a_zlib(void);

// differs between the architectures of a platform: otok dispatches it
#include "tiny_a_arch.h"
#endif
