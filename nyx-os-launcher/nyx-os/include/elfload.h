// include/elfload.h  –  ELF32 loader for ring-3 user programs.
#ifndef ELFLOAD_H
#define ELFLOAD_H

#include <stdint.h>
#include <paging.h>

// The reserved user-mode slot and the virtual address its programs are
// linked at.  cpu/task.c reserves slot 1 for user programs; the user
// linker script (user/user.ld) links at exactly this base.
#define TASK_USER_SLOT   1
#define USER_LINK_BASE   (TASK_PRIVATE_BASE + TASK_PRIVATE_SIZE)  // 0xC1000000

// The loader maps one dedicated page just below the user stack and
// copies the command line there, so user programs can read argv.
// (14 MiB into the user slot's 15 MiB region — clear of both the
// program image and the 256 KiB stack below the 15 MiB mark, so it
// never collides no matter how big the program grows.)
#define USER_ARGS_BASE   (USER_LINK_BASE + 0x00E00000u)
#define USER_ARGS_MAX    512

// Load the embedded ELF32 program `elf` (len bytes) into the reserved
// user slot's private address space (mapping each PT_LOAD segment at
// its link-time vaddr, plus a user stack), then create the ring-3
// task.  Returns the task id (TASK_USER_SLOT) or -1 on error.  If a
// user task already exists it is killed first (relaunch).  `args` is
// copied to USER_ARGS_BASE (may be NULL for no args).
int user_exec(const unsigned char* elf, uint32_t len, const char* name,
              const char* args);

#endif
