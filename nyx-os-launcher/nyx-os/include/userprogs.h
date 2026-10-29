// include/userprogs.h  –  embedded ring-3 user programs (built from
// user/*.c at compile time; generated table in cpu/userprogs.c).
#ifndef USERPROGS_H
#define USERPROGS_H

#include <stdint.h>

typedef struct {
    const char*          name;   // program name (`user`, `userfault`)
    const unsigned char* data;   // ELF32 bytes
    uint32_t             len;    // byte count
} userprog_t;

extern const userprog_t userprogs[];
extern const int         userprog_count;

#endif
