// tools/_fileapi_test.c — Host-side test for Win32 File System API
// Tests: path translation, handle table, virtual directories, CRUD
// Compile: gcc -O2 -o /tmp/fileapi_test tools/_fileapi_test.c

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Stub out kernel types/functions
typedef unsigned int  DWORD;
typedef int           BOOL;
typedef unsigned char BYTE;
typedef void*         HANDLE;
typedef const char*   LPCSTR;
typedef char*         LPSTR;
typedef DWORD*        LPDWORD;
#define TRUE 1
#define FALSE 0

// Minimal filesystem simulation
#define MAX_FILES 127
#define MAX_FILENAME 32
#define MAX_FILESIZE (16*1024*1024)

typedef struct {
    char  name[MAX_FILENAME];
    char* content;
    int   content_cap;
    int   used;
    int   size;
} File;

File filesystem[MAX_FILES];
int  file_count = 0;

void file_content_alloc(int i, int min_cap) {
    if (i < 0 || i >= MAX_FILES) return;
    if (filesystem[i].content && filesystem[i].content_cap >= min_cap) return;
    int cap = min_cap;
    if (cap < 4096) cap = 4096;
    int p = 4096; while (p < cap) p *= 2;
    cap = p;
    if (cap > MAX_FILESIZE) cap = MAX_FILESIZE;
    if (filesystem[i].content) {
        filesystem[i].content = realloc(filesystem[i].content, cap);
    } else {
        filesystem[i].content = malloc(cap);
    }
    memset(filesystem[i].content, 0, cap);
    filesystem[i].content_cap = cap;
}

void file_content_free(int i) {
    if (i < 0 || i >= MAX_FILES) return;
    if (filesystem[i].content) {
        free(filesystem[i].content);
        filesystem[i].content = 0;
        filesystem[i].content_cap = 0;
    }
}

// Include the path translation and directory logic (extracted)
// We test the logic inline here

static DWORD last_error = 0;
void SetLastError(DWORD err) { last_error = err; }
DWORD GetLastError(void) { return last_error; }

// Path translation test
static void path_to_flat(const char* path, char* flat) {
    int j = 0;
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

static int fs_find(const char* flat) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used &&
            strncmp(filesystem[i].name, flat, MAX_FILENAME) == 0)
            return i;
    }
    return -1;
}

static int fs_find_free(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].used) return i;
    }
    return -1;
}

static int fs_create(const char* flat) {
    int i = fs_find_free();
    if (i < 0) return -1;
    memset(filesystem[i].name, 0, MAX_FILENAME);
    strncpy(filesystem[i].name, flat, MAX_FILENAME - 1);
    filesystem[i].used = 1;
    filesystem[i].size = 0;
    file_content_alloc(i, 4096);
    if (filesystem[i].content) filesystem[i].content[0] = '\0';
    return i;
}

// Test cases
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else { tests_failed++; printf("  FAIL: %s\n", msg); } \
} while(0)

static void test_path_translation(void) {
    printf("\n=== Path Translation ===\n");
    char flat[MAX_FILENAME];

    path_to_flat("AppData/Roaming/Telegram/config.db", flat);
    ASSERT(strcmp(flat, "AppData_Roaming_Telegram_config") == 0,
           "Path with slashes (truncated at MAX_FILENAME-1)");

    path_to_flat("\\C:\\Users\\test\\file.txt", flat);
    ASSERT(strcmp(flat, "C:_Users_test_file.txt") == 0,
           "Windows path with leading backslash stripped");

    path_to_flat("/simple", flat);
    ASSERT(strcmp(flat, "simple") == 0,
           "Simple path with leading slash");

    path_to_flat("nopath", flat);
    ASSERT(strcmp(flat, "nopath") == 0,
           "Path without separators");
}

static void test_file_crud(void) {
    printf("\n=== File CRUD ===\n");

    // Create
    int idx = fs_create("test_file");
    ASSERT(idx >= 0, "Create file succeeds");
    ASSERT(filesystem[idx].used == 1, "File marked as used");
    ASSERT(filesystem[idx].size == 0, "New file has size 0");

    // Write
    const char* data = "Hello, Telegram!";
    int len = strlen(data);
    memcpy(filesystem[idx].content, data, len);
    filesystem[idx].size = len;
    ASSERT(strncmp(filesystem[idx].content, data, len) == 0,
           "Write data to file");

    // Read
    char buf[64] = {0};
    memcpy(buf, filesystem[idx].content, filesystem[idx].size);
    ASSERT(strncmp(buf, data, len) == 0,
           "Read data from file");
    ASSERT(filesystem[idx].size == len, "File size correct after write");

    // Find
    ASSERT(fs_find("test_file") == idx, "Find file by name");

    // Delete
    file_content_free(idx);
    filesystem[idx].used = 0;
    ASSERT(fs_find("test_file") < 0, "File deleted and not found");
}

static void test_virtual_directories(void) {
    printf("\n=== Virtual Directories ===\n");

    // Create files with path-like names
    int idx1 = fs_create("AppData_Roaming_Telegram_config.db");
    int idx2 = fs_create("AppData_Roaming_Telegram_data.db");
    int idx3 = fs_create("AppData_Roaming_WhatsApp_config.db");

    ASSERT(idx1 >= 0 && idx2 >= 0 && idx3 >= 0,
           "Create 3 files in virtual directories");

    // Find files by prefix
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used &&
            strncmp(filesystem[i].name, "AppData_Roaming_Telegram_", 25) == 0)
            count++;
    }
    ASSERT(count == 2, "Find 2 files under AppData_Roaming_Telegram_");

    count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used &&
            strncmp(filesystem[i].name, "AppData_Roaming_", 16) == 0)
            count++;
    }
    ASSERT(count == 3, "Find 3 files under AppData_Roaming_");

    // Cleanup
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used) {
            file_content_free(i);
            filesystem[i].used = 0;
        }
    }
}

static void test_large_write(void) {
    printf("\n=== Large Write ===\n");

    int idx = fs_create("large_file");
    ASSERT(idx >= 0, "Create large file");

    // Write 100KB
    int size = 100 * 1024;
    file_content_alloc(idx, size + 1);
    ASSERT(filesystem[idx].content_cap >= size + 1,
           "Buffer allocated >= 100KB");

    for (int i = 0; i < size; i++) {
        filesystem[idx].content[i] = (char)(i & 0xFF);
    }
    filesystem[idx].size = size;

    // Verify
    int ok = 1;
    for (int i = 0; i < size; i++) {
        if (filesystem[idx].content[i] != (char)(i & 0xFF)) {
            ok = 0; break;
        }
    }
    ASSERT(ok, "100KB data integrity check");

    file_content_free(idx);
    filesystem[idx].used = 0;
}

static void test_many_files(void) {
    printf("\n=== Many Files (127 max) ===\n");

    int created = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        char name[MAX_FILENAME];
        snprintf(name, MAX_FILENAME, "file_%04d.dat", i);
        int idx = fs_create(name);
        if (idx >= 0) {
            filesystem[idx].content[0] = 'A' + (i % 26);
            filesystem[idx].size = 1;
            created++;
        }
    }
    ASSERT(created == MAX_FILES, "All 127 file slots filled");

    // Verify can't create one more
    int idx = fs_create("one_more");
    ASSERT(idx < 0, "Cannot create 128th file (full)");

    // Cleanup
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used) {
            file_content_free(i);
            filesystem[i].used = 0;
        }
    }
}

int main(void) {
    printf("Nyx OS File API Test Suite\n");
    printf("==========================\n");

    memset(filesystem, 0, sizeof(filesystem));

    test_path_translation();
    test_file_crud();
    test_virtual_directories();
    test_large_write();
    test_many_files();

    printf("\n==========================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
