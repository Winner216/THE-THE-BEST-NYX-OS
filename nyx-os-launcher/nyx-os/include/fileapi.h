#ifndef FILEAPI_H
#define FILEAPI_H

// ─── Win32 File System API for Nyx OS ───────────────────────────
// Implements CreateFile, ReadFile, WriteFile, DeleteFile,
// FindFirstFile/FindNextFile, and directory operations on top of
// Nyx OS's flat filesystem[] array.
//
// Directories are virtual: the path "AppData/Roaming/Telegram/file"
// maps to the flat filename "AppData_Roaming_Telegram_file" in the
// filesystem[] array.  Directory entries are tracked separately in
// a small directory table so FindFirstFile/FindNextFile can enumerate
// children of a given virtual path.

#include <stdint.h>

#ifndef WINAPI_H
typedef unsigned int   UINT;
typedef unsigned int   DWORD;
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef void*          HANDLE;
typedef void*          LPVOID;
typedef const char*    LPCSTR;
typedef char*          LPSTR;
typedef DWORD*         LPDWORD;
typedef long           LONG;
typedef unsigned long  SIZE_T;
typedef unsigned short WORD;
#define TRUE  1
#define FALSE 0
#endif

#ifndef DWORD_PTR
typedef unsigned long DWORD_PTR;
#endif
#ifndef UINT_PTR
typedef unsigned long UINT_PTR;
#endif

// ─── File access / creation flags ───────────────────────────────
#define GENERIC_READ        0x80000000
#define GENERIC_WRITE       0x40000000
#define GENERIC_EXECUTE     0x20000000
#define GENERIC_ALL         0x10000000

#define CREATE_NEW          1
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define OPEN_ALWAYS         4
#define TRUNCATE_EXISTING   5

// File attributes
#define FILE_ATTRIBUTE_NORMAL   0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_SYSTEM    0x00000004
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020

// File share modes
#define FILE_SHARE_READ     0x00000001
#define FILE_SHARE_WRITE    0x00000002

// File seek origins
#define FILE_BEGIN          0
#define FILE_CURRENT        1
#define FILE_END            2

// Error codes
#define ERROR_SUCCESS              0
#define ERROR_FILE_NOT_FOUND       2
#define ERROR_PATH_NOT_FOUND       3
#define ERROR_ACCESS_DENIED        5
#define ERROR_INVALID_HANDLE       6
#define ERROR_NOT_ENOUGH_MEMORY    8
#define ERROR_ALREADY_EXISTS       183
#define ERROR_DIR_NOT_EMPTY        145
#define ERROR_INVALID_NAME         123

// GetFileAttributes return value for invalid
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

// ─── WIN32_FIND_DATA ────────────────────────────────────────────
typedef struct {
    DWORD dwFileAttributes;
    DWORD ftCreationTime;
    DWORD ftLastAccessTime;
    DWORD ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    char  cFileName[260];
    char  cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA* LPWIN32_FIND_DATAA;

// ─── File API Functions ─────────────────────────────────────────

// CreateFile / ReadFile / WriteFile / CloseHandle
HANDLE winapi_create_file_a(LPCSTR lpFileName, DWORD dwDesiredAccess,
                            DWORD dwShareMode, void* lpSecurityAttributes,
                            DWORD dwCreationDisposition,
                            DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

BOOL winapi_read_file(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                      LPDWORD lpNumberOfBytesRead, void* lpOverlapped);

BOOL winapi_write_file(HANDLE hFile, const void* lpBuffer,
                       DWORD nNumberOfBytesToWrite,
                       LPDWORD lpNumberOfBytesWritten, void* lpOverlapped);

// File positioning
DWORD winapi_set_file_pointer(HANDLE hFile, LONG lDistanceToMove,
                              LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod);

DWORD winapi_get_file_size(HANDLE hFile, LPDWORD lpFileSizeHigh);

// File deletion and metadata
BOOL winapi_delete_file_a(LPCSTR lpFileName);
DWORD winapi_get_file_attributes_a(LPCSTR lpFileName);
BOOL winapi_set_file_attributes_a(LPCSTR lpFileName, DWORD dwFileAttributes);

// Directory operations
BOOL winapi_create_directory_a(LPCSTR lpPathName, void* lpSecurityAttributes);
BOOL winapi_remove_directory_a(LPCSTR lpPathName);

// Directory enumeration
HANDLE winapi_find_first_file_a(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL   winapi_find_next_file_a(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL   winapi_find_close(HANDLE hFindFile);

// Current directory
BOOL winapi_get_current_directory_a(DWORD nBufferLength, LPSTR lpBuffer);
BOOL winapi_set_current_directory_a(LPCSTR lpPathName);

// Temp path
DWORD winapi_get_temp_path_a(DWORD nBufferLength, LPSTR lpBuffer);

// Module path
DWORD winapi_get_module_file_name_a(HANDLE hModule, LPSTR lpFilename, DWORD nSize);

// Move / copy file
BOOL winapi_move_file_a(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
BOOL winapi_copy_file_a(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists);

#endif /* FILEAPI_H */
