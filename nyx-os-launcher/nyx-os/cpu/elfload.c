// cpu/elfload.c  –  ELF32 user-program loader.
//
// Turns an embedded ELF32 executable (built from user/*.c with the
// user linker script at USER_LINK_BASE) into a RUNNING ring-3 task:
//   1. validate the ELF header (32-bit, EM_386, ET_EXEC)
//   2. for each PT_LOAD segment, map fresh pages in the user task's
//      private region at the segment's link-time vaddr and copy the
//      file bytes in (zeroing the .bss tail)
//   3. map a user stack near the top of the region
//   4. task_create_user() — whose fake frame iret's to ring 3
//
// The copies must run under the USER task's page directory: the fresh
// frames are mapped only in task_pds[TASK_USER_SLOT], which is not the
// CR3 of whoever invokes user_exec() (the shell/main task).  The
// kernel image is identity-mapped in every per-task PD, so executing
// under the user task's CR3 for the duration of the copy is safe; we
// save/restore CR3 around it with interrupts masked.
#include <elfload.h>
#include <task.h>
#include <paging.h>
#include <stdint.h>
#include <stddef.h>

// ─── Minimal ELF32 structures (no elf.h in a freestanding kernel) ─
#define ELF_MAGIC   0x464C457Fu        // "\x7fELF"
#define ELF_CLASS32 1
#define ELF_EM_386  3
#define ELF_ET_EXEC 2
#define PT_LOAD     1

typedef struct {
    uint32_t e_ident[4];   // 0x00..0x0F (magic, class, data, version...)
    uint16_t e_type;       // 0x10
    uint16_t e_machine;    // 0x12
    uint32_t e_version;    // 0x14
    uint32_t e_entry;      // 0x18
    uint32_t e_phoff;      // 0x1C
    uint32_t e_shoff;      // 0x20
    uint32_t e_flags;      // 0x24
    uint16_t e_ehsize;     // 0x28
    uint16_t e_phentsize;  // 0x2A
    uint16_t e_phnum;      // 0x2C
    uint16_t e_shentsize;  // 0x2E
    uint16_t e_shnum;      // 0x30
    uint16_t e_shstrndx;   // 0x32
} __attribute__((packed)) elf32_hdr_t;

typedef struct {
    uint32_t p_type;       // 0
    uint32_t p_offset;     // 4
    uint32_t p_vaddr;      // 8
    uint32_t p_paddr;      // 12
    uint32_t p_filesz;     // 16
    uint32_t p_memsz;      // 20
    uint32_t p_flags;      // 24
    uint32_t p_align;      // 28
} __attribute__((packed)) elf32_phdr_t;

// The e_ident field is 16 bytes; our struct starts the words at 0x10,
// so byte 4 (EI_CLASS) is the low byte of e_type's first byte... to
// keep this readable we read the class from the raw bytes instead.
static int elfload_valid(const unsigned char* e, uint32_t len) {
    if (len < 52) return 0;                       // header too small
    uint32_t magic = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                     ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
    if (magic != ELF_MAGIC) return 0;
    if (e[4] != ELF_CLASS32) return 0;            // EI_CLASS
    if (e[5] != 1) return 0;                      // EI_DATA: little-endian
    if (e[6] != 1) return 0;                      // EI_VERSION
    const elf32_hdr_t* h = (const elf32_hdr_t*)e;
    if (h->e_machine != ELF_EM_386) return 0;
    if (h->e_type != ELF_ET_EXEC) return 0;
    if (h->e_phoff == 0 || h->e_phentsize < 32 || h->e_phnum == 0) return 0;
    return 1;
}

// Copy `n` bytes src→dst; zero `z` bytes after them (bss).  Runs under
// the user task's CR3 — see the header comment.
static void loader_copy(uint32_t dst, const unsigned char* src,
                        uint32_t n, uint32_t z) {
    for (uint32_t i = 0; i < n; i++)
        *(volatile uint8_t*)(dst + i) = src[i];
    for (uint32_t i = 0; i < z; i++)
        *(volatile uint8_t*)(dst + n + i) = 0;
}

// ─── Loader entry ────────────────────────────────────────────────
int user_exec(const unsigned char* elf, uint32_t len, const char* name,
              const char* args) {
    if (!elfload_valid(elf, len)) return -1;
    const elf32_hdr_t* h = (const elf32_hdr_t*)elf;

    // Relaunch: kill any previous user task (slot 1 is reserved).
    task_kill(TASK_USER_SLOT);

    // Map each PT_LOAD segment at its link-time vaddr.
    for (uint32_t i = 0; i < h->e_phnum; i++) {
        const uint32_t off = h->e_phoff + i * h->e_phentsize;
        if (off + 32 > len) return -1;
        const elf32_phdr_t* ph = (const elf32_phdr_t*)(elf + off);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        uint32_t va_page = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t end     = ph->p_vaddr + ph->p_memsz;
        uint32_t pages   = (end - va_page + PAGE_SIZE - 1) / PAGE_SIZE;
        // Map with permissions matching the ELF segment flags.
        // ELF p_flags: PF_X=1 PF_W=2 PF_R=4.
        // x86-32 page flags: PAGING_W=2 PAGING_U=4.
        // Map as RW+U initially so loader_copy can write the bytes,
        // then tighten for execute-only segments after copy.
        uint32_t pgf = PAGING_W | PAGING_U;   // default: RW + user
        if (!paging_map_user_va(TASK_USER_SLOT, va_page, (int)pages))
            return -1;                            // out of frames / bad range

        uint32_t n = ph->p_filesz;                // bytes to copy
        uint32_t z = ph->p_memsz - ph->p_filesz;  // bss to zero
        if (ph->p_offset + n > len) n = len > ph->p_offset ? len - ph->p_offset : 0;
        // Copy under the user task's CR3 (the frames are only visible
        // there).  loader_copy is a plain loop — safe to call with the
        // user PD active because the kernel image is identity-mapped.
        uint32_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
        asm volatile("cli");
        asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
        loader_copy(ph->p_vaddr, elf + ph->p_offset, n, z);
        asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
        asm volatile("sti");
        // After copy, tighten permissions: execute-only segments
        // (PF_X set, PF_W clear) get read-only+user so user code
        // can't self-modify.  Writable segments stay RW.
        if ((ph->p_flags & 1) && !(ph->p_flags & 2)) {
            // Execute-only: mark read-only + user
            paging_set_user_va_flags(TASK_USER_SLOT, va_page, (int)pages, PAGING_U);
        }
    }

    // User stack: 256 KiB near the top of the user slot's region.
    const uint32_t stack_top = USER_LINK_BASE + 0x00F00000u;   // 15 MiB in
    const uint32_t stack_pages = 64;                           // 256 KiB
    uint32_t stack_va = stack_top - stack_pages * PAGE_SIZE;
    if (!paging_map_user_va(TASK_USER_SLOT, stack_va, (int)stack_pages))
        return -1;

    // Args page: one dedicated page just below the stack; copy the
    // command line there (if any) so the user program can read argv.
    if (paging_map_user_va(TASK_USER_SLOT, USER_ARGS_BASE, 1)) {
        char* dst = (char*)USER_ARGS_BASE;
        int n = 0;
        if (args) while (args[n] && n < USER_ARGS_MAX - 1) n++;
        // Copy under the user task's CR3 (the frame is only visible there).
        uint32_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r" (saved_cr3));
        asm volatile("cli");
        asm volatile("mov %0, %%cr3" :: "r" (paging_task_pd(TASK_USER_SLOT)));
        for (int i = 0; i < n; i++) dst[i] = args[i];
        dst[n] = '\0';
        asm volatile("mov %0, %%cr3" :: "r" (saved_cr3));
        asm volatile("sti");
    }

    return task_create_user(name, h->e_entry, stack_top);
}
