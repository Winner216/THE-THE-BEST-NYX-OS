// Shim assert.h — __assert_fail is implemented in nyx_doom.c (prints
// and exits).  Can be included with or without NDEBUG.
#ifndef NYX_SHIM_ASSERT_H
#define NYX_SHIM_ASSERT_H

void __assert_fail(const char* expr, const char* file, unsigned line,
                   const char* func) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) \
    ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#endif
