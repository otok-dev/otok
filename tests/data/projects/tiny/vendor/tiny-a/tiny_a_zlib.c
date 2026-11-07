#include "tiny_a.h"
#include <zlib.h>

// in an object of its own: consumers that do not call it need no libz
int tiny_a_zlib(void) { return zlibVersion()[0] == '1'; }
