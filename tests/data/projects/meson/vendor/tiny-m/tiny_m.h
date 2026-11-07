#pragma once

// a DLL exports it (an import library is made only for what it exports)
#if defined(_WIN32) && defined(TINY_M_BUILD) && !defined(TINY_M_STATIC)
#   define TINY_M_API __declspec(dllexport)
#else
#   define TINY_M_API
#endif

TINY_M_API int tiny_m(void);
