// cpu/fileapi.c — Win32 File System API for Nyx OS
//
// Implements the standard Windows file I/O functions on top of
// Nyx OS's flat filesystem[] array (cpu/fs.c, include/fs.h).
//
// Virtual directory layer:
//   "AppData/Roaming/Telegram Desktop/config.db"
//   → flat filename "AppData_Roaming_Telegram Desktop_config.db"
//
// Path separators (/ and \) are replaced with '_' in the flat name.
// The first component before any separator is treated as the root
// directory.  This preserves Telegram's directory structure as a
// naming convention without modifying the kernel filesystem.
//
// File handles use a small handle table (32 slots) that tracks
// the filesystem[] index, current seek position, and access mode.

#include <fileapi.h>
#include <fs.h>
#include <heap.h>
#include <string.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

// ─── Last error (per-thread, simplified to global) ──────
static DWORD last_error = 0;
void SetLastError(DWORD err) { last_error = err; }
DWORD GetLastError(void) { return last_error; }

// ─── Path translation ───────────────────────────────────────────
// Convert a Win32 path to a flat filesystem[] filename.
// Replaces / and \ with _.  The result is written to `flat`
// (must be at least MAX_FILENAME bytes).

static void path_to_flat(const char* path, char* flat) {
    int j = 0;
    // Skip leading slashes
    while (*path == '/' || *path == '\\') path++;
    while (*path && j < MAX_FILENAME - 1) {
        if (*path == '/' || *path == '\\')
            flat[j++] = '_';
        else
            flat[j++] = *path;
        path++;
    }
    flat[j] = '\0';
}

// Convert a flat filename back to a path-like string (replace _ with /).
static void flat_to_path(const char* flat, char* path, int maxlen) {
    int j = 0;
    while (*flat && j < maxlen - 1) {
        path[j++] = *flat;
        flat++;
    }
    path[j] = '\0';
}

// Find filesystem[] index by flat name, or -1 if not found.
static int fs_find(const char* flat) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used &&
            strncmp(filesystem[i].name, flat, MAX_FILENAME) == 0)
            return i;
    }
    return -1;
}

// Find a free filesystem[] slot, or -1 if full.
static int fs_find_free(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) return i;
    }
    return -1;
}

// Create a file in the filesystem[] array.
static int fs_create(const char* flat) {
    int i = fs_find_free();
    if (i < 0) return -1;
    memset(filesystem[i].name, 0, MAX_FILENAME);
    strncpy(filesystem[i].name, flat, MAX_FILENAME - 1);
    filesystem[i].used = 1;
    filesystem[i].size = 0;
    file_content_alloc(i, 4096);
    if (filesystem[i].content)
        filesystem[i].content[0] = '\0';
    return i;
}

// ─── Directory virtual layer ────────────────────────────────────
// Directories are tracked in a small table.  A directory is "created"
// when CreateDirectory is called, or implicitly when a file is created
// with a path containing separators.  Directories are never stored in
// filesystem[] — they exist only in this table for enumeration.

#define MAX_DIRS 64
#define MAX_DIRNAME 128

typedef struct {
    int   used;
    char  name[MAX_DIRNAME];  // flat name of directory (e.g. "AppData_Roaming")
} DirEntry;

static DirEntry dir_table[MAX_DIRS];
static int dir_count = 0;

// Ensure all ancestor directories of `path` exist.
// "AppData/Roaming/Telegram Desktop" ensures:
//   dir "AppData" exists
//   dir "AppData_Roaming" exists
//   dir "AppData_Roaming_Telegram Desktop" exists
static void ensure_dirs(const char* path) {
    char flat[MAX_FILENAME];
    int depth = 0;

    while (*path && depth < MAX_DIRNAME - 1) {
        if (*path == '/' || *path == '\\') {
            // End of a path component — register this directory
            flat[depth] = '\0';
            if (depth > 0) {
                // Check if already in dir_table
                int found = 0;
                for (int i = 0; i < dir_count; i++) {
                    if (dir_table[i].used &&
                        strcmp(dir_table[i].name, flat) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found && dir_count < MAX_DIRS) {
                    dir_table[dir_count].used = 1;
                    strncpy(dir_table[dir_count].name, flat, MAX_DIRNAME - 1);
                    dir_count++;
                }
            }
        } else {
            flat[depth++] = *path;
        }
        path++;
    }
    // Final component (the file itself, not a directory)
    // We don't register the file as a directory
}

// ─── File handle table ──────────────────────────────────────────
#define MAX_FILE_HANDLES 32

typedef struct {
    int   in_use;
    int   fs_idx;       // index into filesystem[]
    int   pos;          // current seek position
    DWORD access;       // GENERIC_READ, GENERIC_WRITE, etc.
    DWORD mode;         // CREATE_ALWAYS, OPEN_EXISTING, etc.
} FileHandle;

static FileHandle file_handles[MAX_FILE_HANDLES];

static FileHandle* alloc_handle(void) {
    for (int i = 0; i < MAX_FILE_HANDLES; i++) {
        if (!file_handles[i].in_use) {
            memset(&file_handles[i], 0, sizeof(FileHandle));
            file_handles[i].in_use = 1;
            return &file_handles[i];
        }
    }
    return NULL;
}

static FileHandle* find_handle(HANDLE h) {
    int idx = (int)(uintptr_t)h;
    if (idx < 0 || idx >= MAX_FILE_HANDLES) return NULL;
    FileHandle* fh = &file_handles[idx];
    return fh->in_use ? fh : NULL;
}

// ─── FindFirstFile / FindNextFile handle table ──────────────────
#define MAX_FIND_HANDLES 8

typedef struct {
    int   in_use;
    char  pattern[MAX_FILENAME];  // search pattern (flat)
    int   pattern_len;            // length of pattern prefix
    int   current_idx;            // current position in filesystem[]
} FindHandle;

static FindHandle find_handles[MAX_FIND_HANDLES];

// ─── Win32 File API Implementations ─────────────────────────────

HANDLE winapi_create_file_a(LPCSTR lpFileName, DWORD dwDesiredAccess,
                            DWORD dwShareMode, void* lpSecurityAttributes,
                            DWORD dwCreationDisposition,
                            DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)dwShareMode; (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes; (void)hTemplateFile;

    if (!lpFileName) return (HANDLE)(uintptr_t)-1;

    char flat[MAX_FILENAME];
    path_to_flat(lpFileName, flat);

    // Ensure ancestor directories exist
    ensure_dirs(lpFileName);

    int idx = fs_find(flat);

    switch (dwCreationDisposition) {
    case CREATE_NEW:
        if (idx >= 0) {
            // File already exists — error
            SetLastError(ERROR_ALREADY_EXISTS);
            return (HANDLE)(uintptr_t)-1;
        }
        idx = fs_create(flat);
        if (idx < 0) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return (HANDLE)(uintptr_t)-1;
        }
        break;

    case CREATE_ALWAYS:
        if (idx < 0) {
            idx = fs_create(flat);
            if (idx < 0) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return (HANDLE)(uintptr_t)-1;
            }
        }
        // Truncate existing file
        if (filesystem[idx].content) {
            filesystem[idx].content[0] = '\0';
        }
        filesystem[idx].size = 0;
        break;

    case OPEN_EXISTING:
        if (idx < 0) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return (HANDLE)(uintptr_t)-1;
        }
        break;

    case OPEN_ALWAYS:
        if (idx < 0) {
            idx = fs_create(flat);
            if (idx < 0) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return (HANDLE)(uintptr_t)-1;
            }
        }
        break;

    case TRUNCATE_EXISTING:
        if (idx < 0) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return (HANDLE)(uintptr_t)-1;
        }
        if (!(dwDesiredAccess & GENERIC_WRITE)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return (HANDLE)(uintptr_t)-1;
        }
        if (filesystem[idx].content) {
            filesystem[idx].content[0] = '\0';
        }
        filesystem[idx].size = 0;
        break;

    default:
        SetLastError(ERROR_INVALID_NAME);
        return (HANDLE)(uintptr_t)-1;
    }

    // Allocate handle
    FileHandle* fh = alloc_handle();
    if (!fh) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return (HANDLE)(uintptr_t)-1;
    }
    fh->fs_idx = idx;
    fh->pos = 0;
    fh->access = dwDesiredAccess;
    fh->mode = dwCreationDisposition;

    return (HANDLE)(uintptr_t)(fh - file_handles);
}

BOOL winapi_read_file(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                      LPDWORD lpNumberOfBytesRead, void* lpOverlapped) {
    (void)lpOverlapped;
    FileHandle* fh = find_handle(hFile);
    if (!fh || !lpBuffer) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    File* f = &filesystem[fh->fs_idx];
    int total = f->size;
    if (total <= 0 && f->content) {
        // Compute length
        total = 0;
        while (total < f->content_cap && f->content[total]) total++;
    }

    int avail = total - fh->pos;
    if (avail <= 0) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return TRUE;  // EOF
    }

    DWORD to_read = nNumberOfBytesToRead;
    if ((int)to_read > avail) to_read = (DWORD)avail;

    if (f->content && fh->pos < f->content_cap) {
        int copy_len = to_read;
        if (fh->pos + copy_len > f->content_cap)
            copy_len = f->content_cap - fh->pos;
        memcpy(lpBuffer, f->content + fh->pos, copy_len);
        // Zero-fill any requested bytes beyond content
        if ((int)to_read > copy_len)
            memset((char*)lpBuffer + copy_len, 0, to_read - copy_len);
    } else {
        memset(lpBuffer, 0, to_read);
    }

    fh->pos += to_read;
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = to_read;
    return TRUE;
}

BOOL winapi_write_file(HANDLE hFile, const void* lpBuffer,
                       DWORD nNumberOfBytesToWrite,
                       LPDWORD lpNumberOfBytesWritten, void* lpOverlapped) {
    (void)lpOverlapped;
    FileHandle* fh = find_handle(hFile);
    if (!fh || !lpBuffer) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Check write access
    if (!(fh->access & GENERIC_WRITE)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    File* f = &filesystem[fh->fs_idx];
    int needed = fh->pos + (int)nNumberOfBytesToWrite;
    if (needed > MAX_FILESIZE) needed = MAX_FILESIZE;

    // Ensure content buffer is large enough
    file_content_alloc(fh->fs_idx, needed + 1);
    if (!f->content) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    // Copy data
    memcpy(f->content + fh->pos, lpBuffer, nNumberOfBytesToWrite);
    fh->pos += nNumberOfBytesToWrite;

    // Update file size
    if (fh->pos > f->size) {
        f->size = fh->pos;
    }
    // Ensure null terminator
    if (fh->pos < f->content_cap)
        f->content[fh->pos] = '\0';

    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = nNumberOfBytesToWrite;
    return TRUE;
}

DWORD winapi_set_file_pointer(HANDLE hFile, LONG lDistanceToMove,
                              LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    (void)lpDistanceToMoveHigh;
    FileHandle* fh = find_handle(hFile);
    if (!fh) {
        SetLastError(ERROR_INVALID_HANDLE);
        return (DWORD)-1;
    }

    File* f = &filesystem[fh->fs_idx];
    int total = f->size;
    if (total <= 0 && f->content) {
        total = 0;
        while (total < f->content_cap && f->content[total]) total++;
    }

    int new_pos = fh->pos;
    switch (dwMoveMethod) {
    case FILE_BEGIN:   new_pos = lDistanceToMove; break;
    case FILE_CURRENT: new_pos = fh->pos + lDistanceToMove; break;
    case FILE_END:     new_pos = total + lDistanceToMove; break;
    }

    if (new_pos < 0) new_pos = 0;
    fh->pos = new_pos;
    return (DWORD)new_pos;
}

DWORD winapi_get_file_size(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    FileHandle* fh = find_handle(hFile);
    if (!fh) {
        SetLastError(ERROR_INVALID_HANDLE);
        return (DWORD)-1;
    }
    if (lpFileSizeHigh) *lpFileSizeHigh = 0;
    return (DWORD)filesystem[fh->fs_idx].size;
}

BOOL winapi_delete_file_a(LPCSTR lpFileName) {
    if (!lpFileName) return FALSE;
    char flat[MAX_FILENAME];
    path_to_flat(lpFileName, flat);
    int idx = fs_find(flat);
    if (idx < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    file_content_free(idx);
    filesystem[idx].used = 0;
    memset(filesystem[idx].name, 0, MAX_FILENAME);
    filesystem[idx].size = 0;
    return TRUE;
}

DWORD winapi_get_file_attributes_a(LPCSTR lpFileName) {
    if (!lpFileName) return INVALID_FILE_ATTRIBUTES;
    char flat[MAX_FILENAME];
    path_to_flat(lpFileName, flat);

    // Check if it's a directory
    for (int i = 0; i < dir_count; i++) {
        if (dir_table[i].used && strcmp(dir_table[i].name, flat) == 0)
            return FILE_ATTRIBUTE_DIRECTORY;
    }

    // Check if it's a file
    int idx = fs_find(flat);
    if (idx >= 0) return FILE_ATTRIBUTE_NORMAL;

    return INVALID_FILE_ATTRIBUTES;
}

BOOL winapi_set_file_attributes_a(LPCSTR lpFileName, DWORD dwFileAttributes) {
    (void)lpFileName; (void)dwFileAttributes;
    // No-op on Nyx OS — files don't have real attributes
    return TRUE;
}

// ─── Directory Operations ───────────────────────────────────────

BOOL winapi_create_directory_a(LPCSTR lpPathName, void* lpSecurityAttributes) {
    (void)lpSecurityAttributes;
    if (!lpPathName) return FALSE;

    char flat[MAX_FILENAME];
    path_to_flat(lpPathName, flat);

    // Register the directory
    for (int i = 0; i < dir_count; i++) {
        if (dir_table[i].used && strcmp(dir_table[i].name, flat) == 0)
            return TRUE;  // Already exists
    }
    if (dir_count >= MAX_DIRS) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    dir_table[dir_count].used = 1;
    strncpy(dir_table[dir_count].name, flat, MAX_DIRNAME - 1);
    dir_count++;

    // Also ensure ancestor directories
    ensure_dirs(lpPathName);

    return TRUE;
}

BOOL winapi_remove_directory_a(LPCSTR lpPathName) {
    if (!lpPathName) return FALSE;
    char flat[MAX_FILENAME];
    path_to_flat(lpPathName, flat);

    // Check if directory is empty (has files or child dirs)
    int flat_len = strlen(flat);
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used &&
            strncmp(filesystem[i].name, flat, flat_len) == 0 &&
            filesystem[i].name[flat_len] == '_') {
            SetLastError(ERROR_DIR_NOT_EMPTY);
            return FALSE;
        }
    }

    // Remove from dir table
    for (int i = 0; i < dir_count; i++) {
        if (dir_table[i].used && strcmp(dir_table[i].name, flat) == 0) {
            dir_table[i].used = 0;
            return TRUE;
        }
    }
    SetLastError(ERROR_PATH_NOT_FOUND);
    return FALSE;
}

// ─── Directory Enumeration ──────────────────────────────────────

HANDLE winapi_find_first_file_a(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    if (!lpFileName || !lpFindFileData) return (HANDLE)(uintptr_t)-1;

    char flat[MAX_FILENAME];
    path_to_flat(lpFileName, flat);

    // Allocate find handle
    int fh_idx = -1;
    for (int i = 0; i < MAX_FIND_HANDLES; i++) {
        if (!find_handles[i].in_use) {
            memset(&find_handles[i], 0, sizeof(FindHandle));
            find_handles[i].in_use = 1;
            fh_idx = i;
            break;
        }
    }
    if (fh_idx < 0) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return (HANDLE)(uintptr_t)-1;
    }

    FindHandle* fh = &find_handles[fh_idx];
    strncpy(fh->pattern, flat, MAX_FILENAME - 1);
    fh->pattern_len = strlen(flat);

    // If pattern ends with *, we match prefix
    int is_wildcard = (flat[fh->pattern_len - 1] == '*');
    if (is_wildcard) {
        fh->pattern_len--;
        fh->pattern[fh->pattern_len] = '\0';
    }

    // Search for first matching file
    fh->current_idx = 0;

    // First try to find matching directories
    for (int i = 0; i < dir_count; i++) {
        if (!dir_table[i].used) continue;
        if (strncmp(dir_table[i].name, fh->pattern, fh->pattern_len) == 0) {
            memset(lpFindFileData, 0, sizeof(WIN32_FIND_DATAA));
            lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            flat_to_path(dir_table[i].name, lpFindFileData->cFileName, 260);
            fh->current_idx = i + 1;  // next search starts here
            return (HANDLE)(uintptr_t)fh_idx;
        }
    }

    // Then try files
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) continue;
        if (strncmp(filesystem[i].name, fh->pattern, fh->pattern_len) == 0) {
            memset(lpFindFileData, 0, sizeof(WIN32_FIND_DATAA));
            lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            lpFindFileData->nFileSizeLow = (DWORD)filesystem[i].size;
            flat_to_path(filesystem[i].name, lpFindFileData->cFileName, 260);
            fh->current_idx = i + 1;
            return (HANDLE)(uintptr_t)fh_idx;
        }
    }

    // Nothing found
    SetLastError(ERROR_FILE_NOT_FOUND);
    find_handles[fh_idx].in_use = 0;
    return (HANDLE)(uintptr_t)-1;
}

BOOL winapi_find_next_file_a(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    int fh_idx = (int)(uintptr_t)hFindFile;
    if (fh_idx < 0 || fh_idx >= MAX_FIND_HANDLES || !find_handles[fh_idx].in_use) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!lpFindFileData) return FALSE;

    FindHandle* fh = &find_handles[fh_idx];

    // Continue searching from where we left off
    // First check remaining directories
    for (int i = fh->current_idx; i < dir_count; i++) {
        if (!dir_table[i].used) continue;
        if (strncmp(dir_table[i].name, fh->pattern, fh->pattern_len) == 0) {
            memset(lpFindFileData, 0, sizeof(WIN32_FIND_DATAA));
            lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            flat_to_path(dir_table[i].name, lpFindFileData->cFileName, 260);
            fh->current_idx = i + 1;
            return TRUE;
        }
    }

    // Then check remaining files
    int start = fh->current_idx;
    if (start < MAX_FILES) {
        for (int i = start; i < MAX_FILES; i++) {
            if (!filesystem[i].used) continue;
            if (strncmp(filesystem[i].name, fh->pattern, fh->pattern_len) == 0) {
                memset(lpFindFileData, 0, sizeof(WIN32_FIND_DATAA));
                lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                lpFindFileData->nFileSizeLow = (DWORD)filesystem[i].size;
                flat_to_path(filesystem[i].name, lpFindFileData->cFileName, 260);
                fh->current_idx = i + 1;
                return TRUE;
            }
        }
    }

    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}

BOOL winapi_find_close(HANDLE hFindFile) {
    int fh_idx = (int)(uintptr_t)hFindFile;
    if (fh_idx >= 0 && fh_idx < MAX_FIND_HANDLES) {
        find_handles[fh_idx].in_use = 0;
    }
    return TRUE;
}

// ─── Current Directory ──────────────────────────────────────────

static char current_dir[MAX_FILENAME] = "C:";

BOOL winapi_get_current_directory_a(DWORD nBufferLength, LPSTR lpBuffer) {
    if (!lpBuffer || nBufferLength < 3) return FALSE;
    strncpy(lpBuffer, current_dir, nBufferLength - 1);
    lpBuffer[nBufferLength - 1] = '\0';
    return TRUE;
}

BOOL winapi_set_current_directory_a(LPCSTR lpPathName) {
    if (!lpPathName) return FALSE;
    strncpy(current_dir, lpPathName, MAX_FILENAME - 1);
    return TRUE;
}

// ─── Temp Path ──────────────────────────────────────────────────

DWORD winapi_get_temp_path_a(DWORD nBufferLength, LPSTR lpBuffer) {
    if (!lpBuffer || nBufferLength < 5) return 0;
    strncpy(lpBuffer, "C:\\Temp", nBufferLength - 1);
    lpBuffer[nBufferLength - 1] = '\0';
    return 8;  // length of "C:\\Temp\0"
}

// ─── Module File Name ──────────────────────────────────────────

DWORD winapi_get_module_file_name_a(HANDLE hModule, LPSTR lpFilename, DWORD nSize) {
    (void)hModule;
    if (!lpFilename || nSize < 4) return 0;
    strncpy(lpFilename, "C:\\nyx.exe", nSize - 1);
    lpFilename[nSize - 1] = '\0';
    return 10;
}

// ─── Move / Copy File ──────────────────────────────────────────

BOOL winapi_move_file_a(LPCSTR lpExistingFileName, LPCSTR lpNewFileName) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE;

    char flat_old[MAX_FILENAME], flat_new[MAX_FILENAME];
    path_to_flat(lpExistingFileName, flat_old);
    path_to_flat(lpNewFileName, flat_new);

    int idx = fs_find(flat_old);
    if (idx < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    // Check if destination already exists
    int dst = fs_find(flat_new);
    if (dst >= 0) {
        // Replace destination
        file_content_free(dst);
        filesystem[dst].used = 0;
    }

    // Rename the file
    memset(filesystem[idx].name, 0, MAX_FILENAME);
    strncpy(filesystem[idx].name, flat_new, MAX_FILENAME - 1);
    return TRUE;
}

BOOL winapi_copy_file_a(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE;

    char flat_old[MAX_FILENAME], flat_new[MAX_FILENAME];
    path_to_flat(lpExistingFileName, flat_old);
    path_to_flat(lpNewFileName, flat_new);

    int src = fs_find(flat_old);
    if (src < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    int dst = fs_find(flat_new);
    if (dst >= 0 && bFailIfExists) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (dst < 0) {
        dst = fs_create(flat_new);
        if (dst < 0) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }

    // Copy content
    File* sf = &filesystem[src];
    File* df = &filesystem[dst];
    int len = sf->size;
    if (len <= 0 && sf->content) {
        len = 0;
        while (len < sf->content_cap && sf->content[len]) len++;
    }

    file_content_alloc(dst, len + 1);
    if (sf->content && df->content) {
        memcpy(df->content, sf->content, len);
        df->content[len] = '\0';
    }
    df->size = len;

    return TRUE;
}

// ─── Serialization of dir_table to/from disk ────────────────────
// Save the virtual directory table as a special file ".__dirs"
// in the filesystem, so directories survive reboots.

// (Stub — would require modifying fs_save_to_disk/fs_load_from_disk)
