// cpu/winapi.c  –  Win32 API compatibility layer for Nyx OS (simplified)
#include <winapi.h>
#include <terminal.h>
#include <task.h>
#include <io.h>
#include <heap.h>
#include <timer.h>
#include <sync.h>
#include <string.h>
#include <crt.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
static int str_eq_local(const char* a, const char* b);

// ─── Winsock stubs ──────────────────────────────────────────────
#include <winsock.h>
#include <socket.h>
#include <dns.h>
#include <tls.h>
#include <openssl.h>
#include <wasapi.h>
#include <fileapi.h>
#include <qt.h>

// ─── C Runtime stubs ────────────────────────────────────────────
size_t strlen(const char* s) { if (!s) return 0; size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char* a, const char* b) { if (!a||!b) return (a==b)?0:(a?1:-1); while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char* a, const char* b, size_t n) { if (!a||!b) return (a==b)?0:(a?1:-1); for(size_t i=0;i<n&&a[i]&&a[i]==b[i];i++){if(i==n-1)return 0;} return (unsigned char)*a-(unsigned char)*b; }
char* strcpy(char* d, const char* s) { if(!d||!s)return d; char*r=d; while((*d++=*s++)); return r; }
char* strncpy(char* d, const char* s, size_t n) { if(!d||!s)return d; size_t i; for(i=0;i<n&&s[i];i++)d[i]=s[i]; for(;i<n;i++)d[i]=0; return d; }
char* strcat(char* d, const char* s) { if(!d||!s)return d; char*r=d; while(*r)r++; while((*r++=*s++)); return d; }
int sprintf(char* s, const char* f, ...) { if(!s||!f)return 0; int n=0; while(f[n]&&n<4095){s[n]=f[n];n++;} s[n]=0; return n; }
int snprintf(char* s, size_t sz, const char* f, ...) { if(!s||!f||sz==0)return 0; size_t n=0; while(f[n]&&n<sz-1){s[n]=f[n];n++;} s[n]=0; return (int)n; }
int sscanf(const char* s, const char* f, ...) { (void)s;(void)f; return 0; }
int atoi(const char* s) { if(!s)return 0; int r=0,sgn=1; while(*s==' '||*s=='\t')s++; if(*s=='-'){sgn=-1;s++;}else if(*s=='+')s++; while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;} return r*sgn; }
long atol(const char* s) { return (long)atoi(s); }
long strtol(const char* s, char** e, int b) { (void)e;(void)b; return s?atol(s):0; }
unsigned long strtoul(const char* s, char** e, int b) { return (unsigned long)strtol(s,e,b); }
void qsort(void* b, size_t n, size_t sz, int (*c)(const void*,const void*)) { (void)b;(void)n;(void)sz;(void)c; }
void* bsearch(const void* k, const void* b, size_t n, size_t sz, int (*c)(const void*,const void*)) { (void)k;(void)b;(void)n;(void)sz;(void)c; return NULL; }
static unsigned int _rand_seed=12345; int rand(void){_rand_seed=_rand_seed*1103515245+12345;return(_rand_seed>>16)&0x7FFF;} void srand(unsigned int s){_rand_seed=s;}
void abort(void){for(;;)asm volatile("hlt");}
void exit(int s){(void)s;abort();}
int system(const char* c){(void)c;return -1;}
int atexit(void (*f)(void)){(void)f;return 0;}
char* getenv(const char* n){(void)n;return NULL;}
void* calloc(size_t n,size_t s){void*p=kmalloc(n*s);if(p){uint8_t*b=(uint8_t*)p;for(size_t i=0;i<n*s;i++)b[i]=0;}return p;}
void* realloc(void* p,size_t s){if(!p)return kmalloc(s);void*n=kmalloc(s);if(n){uint8_t*d=(uint8_t*)n;uint8_t*sr=(uint8_t*)p;for(size_t i=0;i<s;i++)d[i]=sr[i];}return n;}

// ─── kernel32.dll implementations ───────────────────────────────

void winapi_exit_process(UINT uExitCode) {
    (void)uExitCode;
    const char* msg = "WINAPI: ExitProcess\n";
    for (; *msg; msg++) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,(uint8_t)*msg); }
    task_exit();
}

DWORD winapi_get_std_handle(DWORD nStdHandle) { (void)nStdHandle; return 1; }

BOOL winapi_write_console_a(HANDLE h, const void* buf, DWORD n, LPDWORD written, void* r) {
    (void)h;(void)r;
    const char* src=(const char*)buf; DWORD cnt=n;
    if(cnt==0){while(src[cnt])cnt++;}
    for(DWORD i=0;i<cnt;i++){if(src[i]!='\r')terminal_putchar(src[i]);}
    if(written)*written=cnt; return TRUE;
}

void winapi_sleep(DWORD ms) { int t=(int)(ms/20);if(t<1)t=1;task_sleep(t); }
DWORD winapi_get_tick_count(void) { return timer_ms(); }
LPVOID winapi_heap_alloc(HANDLE h, DWORD f, SIZE_T s) { (void)h;(void)f; return kmalloc(s); }
BOOL winapi_heap_free(HANDLE h, DWORD f, LPVOID p) { (void)h;(void)f; if(p)kfree(p); return TRUE; }
HMODULE winapi_get_module_handle_a(LPCSTR n) { (void)n; return (HMODULE)0x10000000; }
void* winapi_get_proc_address(HMODULE h, LPCSTR n) { (void)h; (void)n; return NULL; }
LPVOID winapi_virtual_alloc(LPVOID a, SIZE_T s, DWORD f, DWORD p) { (void)a;(void)f;(void)p; return kmalloc(s); }
BOOL winapi_virtual_free(LPVOID a, SIZE_T s, DWORD f) { (void)s;(void)f; if(a)kfree(a); return TRUE; }
LPSTR winapi_get_command_line_a(void) { return "pe_program"; }

// ─── Threading & Sync ───────────────────────────────────────────

HANDLE winapi_create_thread(void* a, SIZE_T s, void* entry, void* p, DWORD f, DWORD* id) {
    (void)a;(void)s;(void)p;(void)f;
    int tid=task_create("wthread",(void(*)(void))entry);
    if(tid<0)return NULL;
    if(id)*id=(DWORD)tid;
    return (HANDLE)(intptr_t)(tid+1);
}
DWORD winapi_get_current_thread_id(void) { return (DWORD)task_current_id(); }
void winapi_exit_thread(DWORD c) { (void)c; task_exit(); }

DWORD winapi_wait_for_single_object(HANDLE h, DWORD ms) {
    if(!h)return 0xFFFFFFFF;
    int handle=(int)(intptr_t)h;
    int obj_type=1;
    if(handle>=1&&handle<=32)obj_type=1;
    else if(handle>32&&handle<=64){obj_type=2;handle-=32;}
    else if(handle>64&&handle<=96){obj_type=3;handle-=64;}
    else{obj_type=4;handle-=96;}
    return wait_for_single_object(handle,ms,obj_type);
}

DWORD winapi_wait_for_multiple_objects(DWORD n, const HANDLE* h, BOOL a, DWORD ms) { (void)a; if(!n||!h)return 0xFFFFFFFF; return winapi_wait_for_single_object(h[0],ms); }
BOOL winapi_close_handle(HANDLE h) { (void)h; return TRUE; }

HANDLE winapi_create_mutex_a(void* a, BOOL own, LPCSTR n) { (void)a;(void)n; int h=mutex_create(n); if(!h)return NULL; if(own)wait_for_single_object(h,0xFFFFFFFF,1); return (HANDLE)(intptr_t)h; }
BOOL winapi_release_mutex(HANDLE h) { if(!h)return FALSE; return mutex_release((int)(intptr_t)h); }

HANDLE winapi_create_semaphore_a(void* a, LONG init, LONG mx, LPCSTR n) { (void)a;(void)n; int h=semaphore_create(init,mx); if(!h)return NULL; return (HANDLE)(intptr_t)(h+32); }
BOOL winapi_release_semaphore(HANDLE h, LONG cnt, LONG* prev) { if(!h)return FALSE; return semaphore_release((int)(intptr_t)h-32,cnt,(int32_t*)prev); }

void winapi_initialize_critical_section(void* cs) { if(!cs)return; *(int*)cs=critical_section_init(); }
void winapi_enter_critical_section(void* cs) { if(!cs)return; critical_section_enter(*(int*)cs); }
BOOL winapi_try_enter_critical_section(void* cs) { if(!cs)return FALSE; return critical_section_try_enter(*(int*)cs); }
void winapi_leave_critical_section(void* cs) { if(!cs)return; critical_section_leave(*(int*)cs); }
void winapi_delete_critical_section(void* cs) { if(!cs)return; critical_section_delete(*(int*)cs); *(int*)cs=0; }

HANDLE winapi_create_event_a(void* a, BOOL mr, BOOL is, LPCSTR n) { (void)a;(void)n; int h=event_create(mr,is); if(!h)return NULL; return (HANDLE)(intptr_t)(h+64); }
BOOL winapi_set_event(HANDLE h) { if(!h)return FALSE; return event_set((int)(intptr_t)h-64); }
BOOL winapi_reset_event(HANDLE h) { if(!h)return FALSE; return event_reset((int)(intptr_t)h-64); }

void winapi_initialize_srw_lock(void* l) { if(l)*(int*)l=0; }
void winapi_acquire_srw_lock_exclusive(void* l) { (void)l; }
void winapi_release_srw_lock_exclusive(void* l) { (void)l; }
void winapi_acquire_srw_lock_shared(void* l) { (void)l; }
void winapi_release_srw_lock_shared(void* l) { (void)l; }
void winapi_initialize_condition_variable(void* l) { if(l)*(int*)l=0; }
BOOL winapi_sleep_condition_variable_cs(void* cv, void* cs, DWORD ms) { (void)cv;(void)cs;(void)ms; return TRUE; }
BOOL winapi_sleep_condition_variable_srw(void* cv, void* sl, DWORD ms) { (void)cv;(void)sl;(void)ms; return TRUE; }
void winapi_wake_condition_variable(void* cv) { (void)cv; }
void winapi_wake_all_condition_variable(void* cv) { (void)cv; }

LONG winapi_interlocked_exchange(volatile LONG* t, LONG v) { LONG o; __asm__ __volatile__("xchg %0,%1":"=r"(o),"+m"(*t):"0"(v):"memory"); return o; }
LONG winapi_interlocked_compare_exchange(volatile LONG* d, LONG x, LONG c) { LONG o=c; __asm__ __volatile__("lock; cmpxchg %2,%1":"+a"(o),"+m"(*d):"r"(x):"memory"); return o; }
LONG winapi_interlocked_increment(volatile LONG* a) { LONG o; __asm__ __volatile__("lock; xaddl %0,%1":"=r"(o),"+m"(*a):"0"((LONG)1):"memory"); return o+1; }
LONG winapi_interlocked_decrement(volatile LONG* a) { LONG o; __asm__ __volatile__("lock; xaddl %0,%1":"=r"(o),"+m"(*a):"0"((LONG)-1):"memory"); return o-1; }
LONG winapi_interlocked_add(volatile LONG* a, LONG v) { LONG o; __asm__ __volatile__("lock; xaddl %0,%1":"=r"(o),"+m"(*a):"r"(v):"memory"); return o+v; }

DWORD winapi_tls_alloc(void) { return 0; }
BOOL winapi_tls_free(DWORD i) { (void)i; return TRUE; }
void* winapi_tls_get_value(DWORD i) { (void)i; return NULL; }
BOOL winapi_tls_set_value(DWORD i, void* v) { (void)i; (void)v; return TRUE; }

// ─── user32.dll ─────────────────────────────────────────────────
int winapi_message_box_a(HWND h, LPCSTR t, LPCSTR c, UINT u) { (void)h;(void)t;(void)c;(void)u; return IDOK; }

// ─── msvcrt.dll ─────────────────────────────────────────────────
int winapi_printf(const char* f, ...) { if(!f)return 0; int n=0; while(f[n])n++; for(int i=0;i<n;i++)terminal_putchar(f[i]); return n; }
int winapi_puts(const char* s) { if(!s)return -1; int n=0; while(*s){terminal_putchar(*s);s++;n++;} terminal_putchar('\n'); return n+1; }
void* winapi_malloc(SIZE_T s) { return kmalloc(s); }
void winapi_free(void* p) { if(p)kfree(p); }
void winapi_exit(int s) { winapi_exit_process((UINT)s); }

// ─── Winsock wrappers ───────────────────────────────────────────

// ─── Missing user32 functions ──────────────────────────────────
HWND winapi_set_focus(HWND hWnd) { (void)hWnd; return hWnd; }
HWND winapi_get_focus(void) { return NULL; }
HWND winapi_get_active_window(void) { return NULL; }
HWND winapi_set_active_window(HWND hWnd) { (void)hWnd; return hWnd; }
HWND winapi_get_foreground_window(void) { return NULL; }
BOOL winapi_set_foreground_window(HWND hWnd) { (void)hWnd; return TRUE; }
int winapi_get_system_metrics(int nIndex) {
    (void)nIndex;
    return 1920;  // stub: return screen width
}
BOOL winapi_system_parameters_info_a(UINT uiAction, UINT uiParam, void* pvParam, UINT fWinIni) {
    (void)uiAction; (void)uiParam; (void)pvParam; (void)fWinIni;
    return TRUE;
}
UINT winapi_register_clipboard_format_a(LPCSTR lpszFormat) {
    (void)lpszFormat;
    return 1;
}


// Timer
static int timer_ids[8];
static int timer_count = 0;
UINT winapi_set_timer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, void* lpTimerFunc) {
    (void)hWnd; (void)uElapse; (void)lpTimerFunc;
    if (timer_count < 8) { timer_ids[timer_count++] = (int)nIDEvent; }
    return (UINT)nIDEvent;
}
BOOL winapi_kill_timer(HWND hWnd, UINT_PTR uIDEvent) { (void)hWnd; (void)uIDEvent; return TRUE; }

// Clipboard
static void* clip_data = NULL;
static UINT clip_format = 0;
BOOL winapi_open_clipboard(HWND hWndNewOwner) { (void)hWndNewOwner; return TRUE; }
BOOL winapi_close_clipboard(void) { return TRUE; }
HANDLE winapi_get_clipboard_data(UINT uFormat) { (void)uFormat; return clip_data; }
HANDLE winapi_set_clipboard_data(UINT uFormat, HANDLE hMem) { clip_format = uFormat; clip_data = hMem; return hMem; }

// Window class registration
static int class_count = 0;
UINT winapi_register_class_a(const void* lpWndClass) { (void)lpWndClass; return (UINT)(++class_count); }
BOOL winapi_unregister_class_a(LPCSTR lpClassName, HINSTANCE hInstance) { (void)lpClassName; (void)hInstance; return TRUE; }

// Clipboard format

// ─── GDI32 wrappers (map to gdi32.c implementations) ──────────
// These thin wrappers forward to the actual GDI functions in gdi32.c
// so the import resolver can find them via winapi_lookup().

extern HDC GetDC(void* hwnd);
extern int ReleaseDC(void* hwnd, HDC hdc);
extern HDC CreateCompatibleDC(HDC hdc);
extern int DeleteDC(HDC hdc);
extern int MoveToEx(HDC hdc, int x, int y, void* lppt);
extern int LineTo(HDC hdc, int x, int y);
extern int Rectangle(HDC hdc, int left, int top, int right, int bottom);
extern int Ellipse(HDC hdc, int left, int top, int right, int bottom);
extern void* CreatePen(int iStyle, int cWidth, uint32_t color);
extern void* CreateSolidBrush(uint32_t color);
extern void* SelectObject(HDC hdc, void* h);
extern int DeleteObject(void* ho);
extern void* GetStockObject(int i);
extern int TextOutA(HDC hdc, int x, int y, const char* lpString, int c);
extern int DrawTextA(HDC hdc, const char* lpchText, int cchText, RECT* lprc, uint32_t format);
extern int GetTextMetricsA(HDC hdc, void* lptm);
extern int GetTextExtentPoint32A(HDC hdc, const char* lpString, int c, POINT* lpsz);
extern int SetTextAlign(HDC hdc, uint32_t fMode);
extern uint32_t GetTextAlign(HDC hdc);
extern int SetTextColor(HDC hdc, uint32_t color);
extern uint32_t GetTextColor(HDC hdc);
extern int SetBkColor(HDC hdc, uint32_t color);
extern uint32_t GetBkColor(HDC hdc);
extern int SetBkMode(HDC hdc, int iBkMode);
extern void* CreateCompatibleBitmap(HDC hdc, int cx, int cy);
extern void* CreateDIBSection(HDC hdc, const void* pbmi, uint32_t usage, void** ppvBits, void* hSection, uint32_t offset);
extern int BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, uint32_t rop);
extern int StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, uint32_t rop);
extern int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom);
extern int SelectClipRgn(HDC hdc, void* hrgn);
extern void* CreateRectRgn(int x1, int y1, int x2, int y2);
extern int SetViewportOrgEx(HDC hdc, int x, int y, POINT* lpPoint);
extern int SetWindowOrgEx(HDC hdc, int x, int y, POINT* lpPoint);
extern int GetViewportOrgEx(HDC hdc, POINT* lpPoint);
extern int GetWindowOrgEx(HDC hdc, POINT* lpPoint);
extern int SaveDC(HDC hdc);
extern int RestoreDC(HDC hdc, int nSavedDC);
extern uint32_t RGB(uint8_t r, uint8_t g, uint8_t b);

// winapi wrappers that forward to gdi32.c
HDC winapi_get_dc(HWND h) { return GetDC(h); }
HDC winapi_get_window_dc(HWND h) { return GetDC(h); }
int winapi_release_dc(HWND h, HDC dc) { return ReleaseDC(h, dc); }
HDC winapi_create_compatible_dc(HDC dc) { return CreateCompatibleDC(dc); }
int winapi_delete_dc(HDC dc) { return DeleteDC(dc); }
int winapi_move_to_ex(HDC dc, int x, int y, void* pt) { return MoveToEx(dc, x, y, pt); }
int winapi_line_to(HDC dc, int x, int y) { return LineTo(dc, x, y); }
int winapi_rectangle(HDC dc, int l, int t, int r, int b) { return Rectangle(dc, l, t, r, b); }
int winapi_ellipse(HDC dc, int l, int t, int r, int b) { return Ellipse(dc, l, t, r, b); }
int winapi_polygon(HDC dc, const POINT* pt, int n) { (void)dc; (void)pt; (void)n; return 1; }
int winapi_polyline(HDC dc, const POINT* pt, int n) { (void)dc; (void)pt; (void)n; return 1; }
void* winapi_create_pen(int s, int w, uint32_t c) { return CreatePen(s, w, c); }
void* winapi_create_solid_brush(uint32_t c) { return CreateSolidBrush(c); }
void* winapi_select_object(HDC dc, void* h) { return SelectObject(dc, h); }
int winapi_delete_object(void* h) { return DeleteObject(h); }
void* winapi_get_stock_object(int i) { return GetStockObject(i); }
int winapi_text_out_a(HDC dc, int x, int y, const char* s, int c) { return TextOutA(dc, x, y, s, c); }
int winapi_draw_text_a(HDC dc, const char* s, int c, RECT* r, uint32_t f) { return DrawTextA(dc, s, c, r, f); }
int winapi_get_text_metrics_a(HDC dc, void* tm) { return GetTextMetricsA(dc, tm); }
int winapi_get_text_extent_point32_a(HDC dc, const char* s, int c, POINT* p) { return GetTextExtentPoint32A(dc, s, c, p); }
int winapi_set_text_align(HDC dc, uint32_t m) { return SetTextAlign(dc, m); }
uint32_t winapi_get_text_align(HDC dc) { return GetTextAlign(dc); }
int winapi_set_text_color(HDC dc, uint32_t c) { return SetTextColor(dc, c); }
uint32_t winapi_get_text_color(HDC dc) { return GetTextColor(dc); }
int winapi_set_bk_color(HDC dc, uint32_t c) { return SetBkColor(dc, c); }
uint32_t winapi_get_bk_color(HDC dc) { return GetBkColor(dc); }
int winapi_set_bk_mode(HDC dc, int m) { return SetBkMode(dc, m); }
void* winapi_create_compatible_bitmap(HDC dc, int w, int h) { return CreateCompatibleBitmap(dc, w, h); }
void* winapi_create_dib_section(HDC dc, const void* pb, uint32_t u, void** pp, void* hs, uint32_t o) { return CreateDIBSection(dc, pb, u, pp, hs, o); }
int winapi_bit_blt(HDC d, int x, int y, int cx, int cy, HDC s, int x1, int y1, uint32_t r) { return BitBlt(d, x, y, cx, cy, s, x1, y1, r); }
int winapi_stretch_blt(HDC d, int xd, int yd, int wd, int hd, HDC s, int xs, int ys, int ws, int hs, uint32_t r) { return StretchBlt(d, xd, yd, wd, hd, s, xs, ys, ws, hs, r); }
int winapi_intersect_clip_rect(HDC dc, int l, int t, int r, int b) { return IntersectClipRect(dc, l, t, r, b); }
int winapi_select_clip_rgn(HDC dc, void* r) { return SelectClipRgn(dc, r); }
void* winapi_create_rect_rgn(int x1, int y1, int x2, int y2) { return CreateRectRgn(x1, y1, x2, y2); }
int winapi_set_viewport_org_ex(HDC dc, int x, int y, POINT* p) { return SetViewportOrgEx(dc, x, y, p); }
int winapi_set_window_org_ex(HDC dc, int x, int y, POINT* p) { return SetWindowOrgEx(dc, x, y, p); }
int winapi_get_viewport_org_ex(HDC dc, POINT* p) { return GetViewportOrgEx(dc, p); }
int winapi_get_window_org_ex(HDC dc, POINT* p) { return GetWindowOrgEx(dc, p); }
int winapi_save_dc(HDC dc) { return SaveDC(dc); }
int winapi_restore_dc(HDC dc, int n) { return RestoreDC(dc, n); }
uint32_t winapi_rgb(uint8_t r, uint8_t g, uint8_t b) { return RGB(r, g, b); }
int winapi_get_device_caps(HDC dc, int i) { (void)dc; (void)i; return 0; }

// ─── str_eq helper ──────────────────────────────────────────────
static int str_eq_local(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── Import lookup table ────────────────────────────────────────
// Use a macro to simplify table entries
#define WP(fn) ((uint32_t)(uintptr_t)(fn))

const winapi_entry_t winapi_table[] = {
    // kernel32.dll - Process
    {"kernel32.dll", "ExitProcess",        WP(winapi_exit_process)},
    {"kernel32.dll", "GetStdHandle",       WP(winapi_get_std_handle)},
    {"kernel32.dll", "WriteConsoleA",      WP(winapi_write_console_a)},
    {"kernel32.dll", "WriteConsoleW",      WP(winapi_write_console_a)},
    {"kernel32.dll", "Sleep",              WP(winapi_sleep)},
    {"kernel32.dll", "GetTickCount",       WP(winapi_get_tick_count)},
    {"kernel32.dll", "HeapAlloc",          WP(winapi_heap_alloc)},
    {"kernel32.dll", "HeapFree",           WP(winapi_heap_free)},
    {"kernel32.dll", "GetModuleHandleA",   WP(winapi_get_module_handle_a)},
    {"kernel32.dll", "GetModuleHandle",    WP(winapi_get_module_handle_a)},
    {"kernel32.dll", "GetProcAddress",     WP(winapi_get_proc_address)},
    {"kernel32.dll", "VirtualAlloc",       WP(winapi_virtual_alloc)},
    {"kernel32.dll", "VirtualFree",        WP(winapi_virtual_free)},
    {"kernel32.dll", "GetCommandLineA",    WP(winapi_get_command_line_a)},
    {"kernel32.dll", "GetCommandLine",     WP(winapi_get_command_line_a)},
    {"kernel32.dll", "CreateThread",       WP(winapi_create_thread)},
    {"kernel32.dll", "GetCurrentThreadId", WP(winapi_get_current_thread_id)},
    {"kernel32.dll", "ExitThread",         WP(winapi_exit_thread)},
    {"kernel32.dll", "WaitForSingleObject",WP(winapi_wait_for_single_object)},
    {"kernel32.dll", "WaitForMultipleObjects",WP(winapi_wait_for_multiple_objects)},
    {"kernel32.dll", "CloseHandle",        WP(winapi_close_handle)},
    {"kernel32.dll", "CreateMutexA",       WP(winapi_create_mutex_a)},
    {"kernel32.dll", "CreateMutex",        WP(winapi_create_mutex_a)},
    {"kernel32.dll", "ReleaseMutex",       WP(winapi_release_mutex)},
    {"kernel32.dll", "CreateSemaphoreA",   WP(winapi_create_semaphore_a)},
    {"kernel32.dll", "CreateSemaphore",    WP(winapi_create_semaphore_a)},
    {"kernel32.dll", "ReleaseSemaphore",   WP(winapi_release_semaphore)},
    {"kernel32.dll", "InitializeCriticalSection", WP(winapi_initialize_critical_section)},
    {"kernel32.dll", "EnterCriticalSection",    WP(winapi_enter_critical_section)},
    {"kernel32.dll", "TryEnterCriticalSection",  WP(winapi_try_enter_critical_section)},
    {"kernel32.dll", "LeaveCriticalSection",    WP(winapi_leave_critical_section)},
    {"kernel32.dll", "DeleteCriticalSection",   WP(winapi_delete_critical_section)},
    {"kernel32.dll", "CreateEventA",       WP(winapi_create_event_a)},
    {"kernel32.dll", "CreateEvent",        WP(winapi_create_event_a)},
    {"kernel32.dll", "SetEvent",           WP(winapi_set_event)},
    {"kernel32.dll", "ResetEvent",         WP(winapi_reset_event)},
    {"kernel32.dll", "InitializeSRWLock",  WP(winapi_initialize_srw_lock)},
    {"kernel32.dll", "AcquireSRWLockExclusive", WP(winapi_acquire_srw_lock_exclusive)},
    {"kernel32.dll", "ReleaseSRWLockExclusive", WP(winapi_release_srw_lock_exclusive)},
    {"kernel32.dll", "AcquireSRWLockShared",    WP(winapi_acquire_srw_lock_shared)},
    {"kernel32.dll", "ReleaseSRWLockShared",    WP(winapi_release_srw_lock_shared)},
    {"kernel32.dll", "InitializeConditionVariable", WP(winapi_initialize_condition_variable)},
    {"kernel32.dll", "SleepConditionVariableCS",    WP(winapi_sleep_condition_variable_cs)},
    {"kernel32.dll", "SleepConditionVariableSRW",   WP(winapi_sleep_condition_variable_srw)},
    {"kernel32.dll", "WakeConditionVariable",       WP(winapi_wake_condition_variable)},
    {"kernel32.dll", "WakeAllConditionVariable",    WP(winapi_wake_all_condition_variable)},
    {"kernel32.dll", "InterlockedExchange",     WP(winapi_interlocked_exchange)},
    {"kernel32.dll", "InterlockedCompareExchange", WP(winapi_interlocked_compare_exchange)},
    {"kernel32.dll", "InterlockedIncrement",    WP(winapi_interlocked_increment)},
    {"kernel32.dll", "InterlockedDecrement",    WP(winapi_interlocked_decrement)},
    {"kernel32.dll", "InterlockedAdd",          WP(winapi_interlocked_add)},
    {"kernel32.dll", "TlsAlloc",          WP(winapi_tls_alloc)},
    {"kernel32.dll", "TlsFree",           WP(winapi_tls_free)},
    {"kernel32.dll", "TlsGetValue",       WP(winapi_tls_get_value)},
    {"kernel32.dll", "TlsSetValue",       WP(winapi_tls_set_value)},

    // user32.dll - Windowing
    {"user32.dll",   "MessageBoxA",        WP(winapi_message_box_a)},
    {"user32.dll",   "MessageBox",         WP(winapi_message_box_a)},
    {"user32.dll",   "MessageBoxW",        WP(winapi_message_box_a)},
    {"user32.dll",   "CreateWindowExA",    WP(winapi_create_window_ex_a)},
    {"user32.dll",   "CreateWindowEx",     WP(winapi_create_window_ex_a)},
    {"user32.dll",   "CreateWindowA",      WP(winapi_create_window_ex_a)},
    {"user32.dll",   "DestroyWindow",      WP(winapi_destroy_window)},
    {"user32.dll",   "ShowWindow",         WP(winapi_show_window)},
    {"user32.dll",   "UpdateWindow",       WP(winapi_update_window)},
    {"user32.dll",   "SetWindowPos",       WP(winapi_set_window_pos)},
    {"user32.dll",   "MoveWindow",         WP(winapi_move_window)},
    {"user32.dll",   "SetWindowTextA",     WP(winapi_set_window_text_a)},
    {"user32.dll",   "SetWindowText",      WP(winapi_set_window_text_a)},
    {"user32.dll",   "GetWindowTextA",     WP(winapi_get_window_text_a)},
    {"user32.dll",   "GetWindowText",      WP(winapi_get_window_text_a)},
    {"user32.dll",   "GetWindowRect",      WP(winapi_get_window_rect)},
    {"user32.dll",   "GetClientRect",      WP(winapi_get_client_rect)},
    {"user32.dll",   "SetWindowLongA",     WP(winapi_set_window_long_a)},
    {"user32.dll",   "SetWindowLong",      WP(winapi_set_window_long_a)},
    {"user32.dll",   "GetWindowLongA",     WP(winapi_get_window_long_a)},
    {"user32.dll",   "GetWindowLong",      WP(winapi_get_window_long_a)},
    {"user32.dll",   "GetMessageA",        WP(winapi_get_message_a)},
    {"user32.dll",   "GetMessage",         WP(winapi_get_message_a)},
    {"user32.dll",   "PeekMessageA",       WP(winapi_peek_message_a)},
    {"user32.dll",   "PeekMessage",        WP(winapi_peek_message_a)},
    {"user32.dll",   "TranslateMessage",   WP(winapi_translate_message)},
    {"user32.dll",   "DispatchMessageA",   WP(winapi_dispatch_message_a)},
    {"user32.dll",   "DispatchMessage",    WP(winapi_dispatch_message_a)},
    {"user32.dll",   "PostMessageA",       WP(winapi_post_message_a)},
    {"user32.dll",   "PostMessage",        WP(winapi_post_message_a)},
    {"user32.dll",   "SendMessageA",       WP(winapi_send_message_a)},
    {"user32.dll",   "SendMessage",        WP(winapi_send_message_a)},
    {"user32.dll",   "PostQuitMessage",    WP(winapi_post_quit_message)},
    {"user32.dll",   "DefWindowProcA",     WP(winapi_def_window_proc_a)},
    {"user32.dll",   "DefWindowProc",      WP(winapi_def_window_proc_a)},
    {"user32.dll",   "GetCursorPos",       WP(winapi_get_cursor_pos)},
    {"user32.dll",   "SetCursorPos",       WP(winapi_set_cursor_pos)},
    {"user32.dll",   "SetCursor",          WP(winapi_set_cursor)},
    {"user32.dll",   "ShowCursor",         WP(winapi_show_cursor)},
    {"user32.dll",   "GetCapture",         WP(winapi_get_capture)},
    {"user32.dll",   "SetCapture",         WP(winapi_set_capture)},
    {"user32.dll",   "ReleaseCapture",     WP(winapi_release_capture)},
    {"user32.dll",   "ClientToScreen",     WP(winapi_client_to_screen)},
    {"user32.dll",   "ScreenToClient",     WP(winapi_screen_to_client)},
    {"user32.dll",   "GetAsyncKeyState",   WP(winapi_get_async_key_state)},
    {"user32.dll",   "GetKeyState",        WP(winapi_get_key_state)},
    {"user32.dll",   "GetKeyNameTextA",    WP(winapi_get_key_name_text_a)},
    {"user32.dll",   "GetDC",              WP(winapi_get_dc)},
    {"user32.dll",   "GetWindowDC",        WP(winapi_get_window_dc)},
    {"user32.dll",   "ReleaseDC",          WP(winapi_release_dc)},
    {"user32.dll",   "BeginPaint",         WP(winapi_begin_paint)},
    {"user32.dll",   "EndPaint",           WP(winapi_end_paint)},
    {"user32.dll",   "InvalidateRect",     WP(winapi_invalidate_rect)},
    {"user32.dll",   "ValidateRect",       WP(winapi_validate_rect)},
    {"user32.dll",   "CreateMenu",         WP(winapi_create_menu)},
    {"user32.dll",   "CreatePopupMenu",    WP(winapi_create_popup_menu)},
    {"user32.dll",   "AppendMenuA",        WP(winapi_append_menu_a)},
    {"user32.dll",   "AppendMenu",         WP(winapi_append_menu_a)},
    {"user32.dll",   "DestroyMenu",        WP(winapi_destroy_menu)},
    {"user32.dll",   "TrackPopupMenu",     WP(winapi_trackPopupMenu)},
    {"user32.dll",   "SetTimer",           WP(winapi_set_timer)},
    {"user32.dll",   "KillTimer",          WP(winapi_kill_timer)},
    {"user32.dll",   "OpenClipboard",      WP(winapi_open_clipboard)},
    {"user32.dll",   "CloseClipboard",     WP(winapi_close_clipboard)},
    {"user32.dll",   "GetClipboardData",   WP(winapi_get_clipboard_data)},
    {"user32.dll",   "SetClipboardData",   WP(winapi_set_clipboard_data)},
    {"user32.dll",   "RegisterClassA",     WP(winapi_register_class_a)},
    {"user32.dll",   "RegisterClass",      WP(winapi_register_class_a)},
    {"user32.dll",   "UnregisterClassA",   WP(winapi_unregister_class_a)},
    {"user32.dll",   "SetFocus",           WP(winapi_set_focus)},
    {"user32.dll",   "GetFocus",           WP(winapi_get_focus)},
    {"user32.dll",   "GetActiveWindow",    WP(winapi_get_active_window)},
    {"user32.dll",   "SetActiveWindow",    WP(winapi_set_active_window)},
    {"user32.dll",   "GetForegroundWindow",WP(winapi_get_foreground_window)},
    {"user32.dll",   "SetForegroundWindow",WP(winapi_set_foreground_window)},
    {"user32.dll",   "GetSystemMetrics",   WP(winapi_get_system_metrics)},
    {"user32.dll",   "SystemParametersInfoA", WP(winapi_system_parameters_info_a)},
    {"user32.dll",   "SystemParametersInfo",  WP(winapi_system_parameters_info_a)},
    {"user32.dll",   "GetDesktopWindow",   WP(winapi_get_module_handle_a)},
    {"user32.dll",   "RegisterClipboardFormatA", WP(winapi_register_clipboard_format_a)},

    // msvcrt.dll
    {"msvcrt.dll",   "printf",             WP(winapi_printf)},
    {"msvcrt.dll",   "puts",               WP(winapi_puts)},
    {"msvcrt.dll",   "malloc",             WP(winapi_malloc)},
    {"msvcrt.dll",   "free",               WP(winapi_free)},
    {"msvcrt.dll",   "exit",               WP(winapi_exit)},
    {"msvcrt.dll",   "_exit",              WP(winapi_exit)},
    {"msvcrt.dll",   "memcpy",             WP(memcpy)},
    {"msvcrt.dll",   "memset",             WP(memset)},
    {"msvcrt.dll",   "memcmp",             WP(memcmp)},
    {"msvcrt.dll",   "memmove",            WP(memmove)},
    {"msvcrt.dll",   "strlen",             WP(strlen)},
    {"msvcrt.dll",   "strcmp",              WP(strcmp)},
    {"msvcrt.dll",   "strcpy",             WP(strcpy)},
    {"msvcrt.dll",   "strncpy",            WP(strncpy)},
    {"msvcrt.dll",   "strcat",             WP(strcat)},
    {"msvcrt.dll",   "sprintf",            WP(sprintf)},
    {"msvcrt.dll",   "snprintf",           WP(snprintf)},
    {"msvcrt.dll",   "sscanf",             WP(sscanf)},
    {"msvcrt.dll",   "atoi",               WP(atoi)},
    {"msvcrt.dll",   "atol",               WP(atol)},
    {"msvcrt.dll",   "strtol",             WP(strtol)},
    {"msvcrt.dll",   "strtoul",            WP(strtoul)},
    {"msvcrt.dll",   "qsort",              WP(qsort)},
    {"msvcrt.dll",   "bsearch",            WP(bsearch)},
    {"msvcrt.dll",   "rand",               WP(rand)},
    {"msvcrt.dll",   "srand",              WP(srand)},
    {"msvcrt.dll",   "calloc",             WP(calloc)},
    {"msvcrt.dll",   "realloc",            WP(realloc)},
    {"msvcrt.dll",   "abort",              WP(abort)},
    {"msvcrt.dll",   "system",             WP(system)},

    // ws2_32.dll
    {"ws2_32.dll",   "WSAStartup",         WP(WSAStartup)},
    {"ws2_32.dll",   "WSACleanup",         WP(WSACleanup)},
    {"ws2_32.dll",   "WSAGetLastError",    WP(WSAGetLastError)},
    {"ws2_32.dll",   "WSASetLastError",    WP(WSASetLastError)},
    {"ws2_32.dll",   "socket",             WP(ws_socket)},
    {"ws2_32.dll",   "closesocket",        WP(ws_closesocket)},
    {"ws2_32.dll",   "connect",            WP(ws_connect)},
    {"ws2_32.dll",   "send",               WP(ws_send)},
    {"ws2_32.dll",   "recv",               WP(ws_recv)},
    {"ws2_32.dll",   "shutdown",           WP(ws_shutdown)},
    {"ws2_32.dll",   "ioctlsocket",        WP(ws_ioctlsocket)},
    {"ws2_32.dll",   "gethostbyname",      WP(ws_gethostbyname)},
    {"ws2_32.dll",   "gethostname",        WP(ws_gethostname)},
    {"ws2_32.dll",   "inet_addr",          WP(ws_inet_addr)},
    {"ws2_32.dll",   "inet_ntoa",          WP(ws_inet_ntoa)},
    {"ws2_32.dll",   "htons",              WP(ws_htons)},
    {"ws2_32.dll",   "ntohs",              WP(ws_ntohs)},
    {"ws2_32.dll",   "htonl",              WP(ws_htonl)},
    {"ws2_32.dll",   "ntohl",              WP(ws_ntohl)},
    {"ws2_32.dll",   "select",             WP(ws_select)},

    // libssl.dll
    {"libssl.dll",   "TLS_client_method",  WP(TLS_client_method)},
    {"libssl.dll",   "SSL_CTX_new",        WP(SSL_CTX_new)},
    {"libssl.dll",   "SSL_CTX_free",       WP(SSL_CTX_free)},
    {"libssl.dll",   "SSL_new",            WP(SSL_new)},
    {"libssl.dll",   "SSL_free",           WP(SSL_free)},
    {"libssl.dll",   "SSL_set_fd",         WP(SSL_set_fd)},
    {"libssl.dll",   "SSL_connect",        WP(SSL_connect)},
    {"libssl.dll",   "SSL_read",           WP(SSL_read)},
    {"libssl.dll",   "SSL_write",          WP(SSL_write)},
    {"libssl.dll",   "SSL_shutdown",       WP(SSL_shutdown)},
    {"libssl.dll",   "SSL_get_error",      WP(SSL_get_error)},
    {"libssl.dll",   "SSL_get_peer_certificate", WP(SSL_get_peer_certificate)},
    {"libssl.dll",   "SSL_get_verify_result", WP(SSL_get_verify_result)},
    {"libssl.dll",   "SSL_set_tlsext_host_name", WP(SSL_set_tlsext_host_name)},
    {"libssl.dll",   "TLSv1_2_method",    WP(TLS_client_method)},
    {"libssl.dll",   "SSLv23_method",     WP(TLS_client_method)},
    {"libssl.dll",   "SSL_CTX_set_cipher_list", WP(SSL_CTX_set_cipher_list)},
    {"libssl.dll",   "SSL_CTX_load_verify_locations", WP(SSL_CTX_load_verify_locations)},
    {"libssl.dll",   "SSL_CTX_set_default_verify_paths", WP(SSL_CTX_set_default_verify_paths)},
    {"libssl.dll",   "SSL_CTX_set_verify", WP(SSL_CTX_set_verify)},
    {"libssl.dll",   "SSL_CTX_set_verify_depth", WP(SSL_CTX_set_verify_depth)},

    // libcrypto.dll
    {"libcrypto.dll", "ERR_get_error",     WP(ERR_get_error)},
    {"libcrypto.dll", "ERR_error_string",  WP(ERR_error_string)},
    {"libcrypto.dll", "ERR_free_strings",  WP(ERR_free_strings)},
    {"libcrypto.dll", "BIO_new_mem_buf",   WP(BIO_new_mem_buf)},
    {"libcrypto.dll", "BIO_read",          WP(BIO_read)},
    {"libcrypto.dll", "BIO_write",         WP(BIO_write)},
    {"libcrypto.dll", "BIO_free",          WP(BIO_free)},
    {"libcrypto.dll", "EVP_sha256",        WP(EVP_sha256)},
    {"libcrypto.dll", "EVP_sha1",          WP(EVP_sha1)},
    {"libcrypto.dll", "EVP_aes_128_cbc",   WP(EVP_aes_128_cbc)},
    {"libcrypto.dll", "EVP_MD_CTX_new",    WP(EVP_MD_CTX_new)},
    {"libcrypto.dll", "EVP_MD_CTX_free",   WP(EVP_MD_CTX_free)},
    {"libcrypto.dll", "EVP_DigestInit",    WP(EVP_DigestInit)},
    {"libcrypto.dll", "EVP_DigestUpdate",  WP(EVP_DigestUpdate)},
    {"libcrypto.dll", "EVP_DigestFinal",   WP(EVP_DigestFinal)},
    {"libcrypto.dll", "EVP_CIPHER_CTX_new", WP(EVP_CIPHER_CTX_new)},
    {"libcrypto.dll", "EVP_CIPHER_CTX_free", WP(EVP_CIPHER_CTX_free)},
    {"libcrypto.dll", "EVP_EncryptInit",   WP(EVP_EncryptInit)},
    {"libcrypto.dll", "EVP_EncryptUpdate", WP(EVP_EncryptUpdate)},
    {"libcrypto.dll", "EVP_DecryptInit",   WP(EVP_DecryptInit)},
    {"libcrypto.dll", "EVP_DecryptUpdate", WP(EVP_DecryptUpdate)},
    {"libcrypto.dll", "HMAC",              WP(HMAC)},
    {"libcrypto.dll", "BN_new",            WP(BN_new)},
    {"libcrypto.dll", "BN_free",           WP(BN_free)},
    {"libcrypto.dll", "BN_bin2bn",         WP(BN_bin2bn)},
    {"libcrypto.dll", "BN_bn2bin",         WP(BN_bn2bin)},
    {"libcrypto.dll", "BN_cmp",            WP(BN_cmp)},
    {"libcrypto.dll", "BN_add",            WP(BN_add)},
    {"libcrypto.dll", "BN_sub",            WP(BN_sub)},
    {"libcrypto.dll", "BN_mul",            WP(BN_mul)},
    {"libcrypto.dll", "BN_mod_exp",        WP(BN_mod_exp)},
    {"libcrypto.dll", "RSA_new",           WP(RSA_new)},
    {"libcrypto.dll", "RSA_free",          WP(RSA_free)},
    {"libcrypto.dll", "RSA_sign",          WP(RSA_sign)},
    {"libcrypto.dll", "RSA_verify",        WP(RSA_verify)},
    {"libcrypto.dll", "EC_KEY_new_by_curve_name", WP(EC_KEY_new_by_curve_name)},
    {"libcrypto.dll", "EC_KEY_free",       WP(EC_KEY_free)},
    {"libcrypto.dll", "X509_free",         WP(X509_free)},

    // winmm.dll - Windows Multimedia (waveOut, waveIn, mixer, timers)
    {"winmm.dll",   "waveOutGetNumDevs",   WP(waveOutGetNumDevs)},
    {"winmm.dll",   "waveOutGetDevCapsA",  WP(waveOutGetDevCapsA)},
    {"winmm.dll",   "waveOutOpen",         WP(waveOutOpen)},
    {"winmm.dll",   "waveOutClose",        WP(waveOutClose)},
    {"winmm.dll",   "waveOutPrepareHeader", WP(waveOutPrepareHeader)},
    {"winmm.dll",   "waveOutUnprepareHeader", WP(waveOutUnprepareHeader)},
    {"winmm.dll",   "waveOutWrite",        WP(waveOutWrite)},
    {"winmm.dll",   "waveOutPause",        WP(waveOutPause)},
    {"winmm.dll",   "waveOutRestart",      WP(waveOutRestart)},
    {"winmm.dll",   "waveOutReset",        WP(waveOutReset)},
    {"winmm.dll",   "waveOutGetPosition",  WP(waveOutGetPosition)},
    {"winmm.dll",   "waveOutSetVolume",    WP(waveOutSetVolume)},
    {"winmm.dll",   "waveOutGetVolume",    WP(waveOutGetVolume)},
    {"winmm.dll",   "waveOutGetID",        WP(waveOutGetID)},
    {"winmm.dll",   "waveOutMessage",      WP(waveOutMessage)},
    {"winmm.dll",   "waveInGetNumDevs",    WP(waveInGetNumDevs)},
    {"winmm.dll",   "waveInGetDevCapsA",   WP(waveInGetDevCapsA)},
    {"winmm.dll",   "waveInOpen",          WP(waveInOpen)},
    {"winmm.dll",   "waveInClose",         WP(waveInClose)},
    {"winmm.dll",   "waveInPrepareHeader", WP(waveInPrepareHeader)},
    {"winmm.dll",   "waveInUnprepareHeader", WP(waveInUnprepareHeader)},
    {"winmm.dll",   "waveInAddBuffer",     WP(waveInAddBuffer)},
    {"winmm.dll",   "waveInStart",         WP(waveInStart)},
    {"winmm.dll",   "waveInStop",          WP(waveInStop)},
    {"winmm.dll",   "waveInReset",         WP(waveInReset)},
    {"winmm.dll",   "waveInGetID",         WP(waveInGetID)},
    {"winmm.dll",   "mixerOpen",           WP(mixerOpen)},
    {"winmm.dll",   "mixerClose",          WP(mixerClose)},
    {"winmm.dll",   "mixerGetNumDevs",     WP(mixerGetNumDevs)},
    {"winmm.dll",   "mixerGetLineInfoA",   WP(mixerGetLineInfoA)},
    {"winmm.dll",   "mixerGetLineControlsA", WP(mixerGetLineControlsA)},
    {"winmm.dll",   "mixerSetControlDetails", WP(mixerSetControlDetails)},
    {"winmm.dll",   "mixerGetControlDetailsA", WP(mixerGetControlDetailsA)},
    {"winmm.dll",   "timeGetTime",         WP(timeGetTime)},
    {"winmm.dll",   "timeBeginPeriod",     WP(timeBeginPeriod)},
    {"winmm.dll",   "timeEndPeriod",       WP(timeEndPeriod)},

    // gdi32.dll - Graphics Device Interface
    {"gdi32.dll",    "GetDC",              WP(winapi_get_dc)},
    {"gdi32.dll",    "GetWindowDC",        WP(winapi_get_window_dc)},
    {"gdi32.dll",    "ReleaseDC",          WP(winapi_release_dc)},
    {"gdi32.dll",    "CreateCompatibleDC", WP(winapi_create_compatible_dc)},
    {"gdi32.dll",    "DeleteDC",           WP(winapi_delete_dc)},
    {"gdi32.dll",    "MoveToEx",           WP(winapi_move_to_ex)},
    {"gdi32.dll",    "LineTo",             WP(winapi_line_to)},
    {"gdi32.dll",    "Rectangle",          WP(winapi_rectangle)},
    {"gdi32.dll",    "Ellipse",            WP(winapi_ellipse)},
    {"gdi32.dll",    "Polygon",            WP(winapi_polygon)},
    {"gdi32.dll",    "Polyline",           WP(winapi_polyline)},
    {"gdi32.dll",    "CreatePen",          WP(winapi_create_pen)},
    {"gdi32.dll",    "CreateSolidBrush",   WP(winapi_create_solid_brush)},
    {"gdi32.dll",    "SelectObject",       WP(winapi_select_object)},
    {"gdi32.dll",    "DeleteObject",       WP(winapi_delete_object)},
    {"gdi32.dll",    "GetStockObject",     WP(winapi_get_stock_object)},
    {"gdi32.dll",    "TextOutA",           WP(winapi_text_out_a)},
    {"gdi32.dll",    "TextOut",            WP(winapi_text_out_a)},
    {"gdi32.dll",    "DrawTextA",          WP(winapi_draw_text_a)},
    {"gdi32.dll",    "DrawText",           WP(winapi_draw_text_a)},
    {"gdi32.dll",    "GetTextMetricsA",    WP(winapi_get_text_metrics_a)},
    {"gdi32.dll",    "GetTextMetrics",     WP(winapi_get_text_metrics_a)},
    {"gdi32.dll",    "GetTextExtentPoint32A", WP(winapi_get_text_extent_point32_a)},
    {"gdi32.dll",    "GetTextExtentPoint32",  WP(winapi_get_text_extent_point32_a)},
    {"gdi32.dll",    "SetTextAlign",       WP(winapi_set_text_align)},
    {"gdi32.dll",    "GetTextAlign",       WP(winapi_get_text_align)},
    {"gdi32.dll",    "SetTextColor",       WP(winapi_set_text_color)},
    {"gdi32.dll",    "GetTextColor",       WP(winapi_get_text_color)},
    {"gdi32.dll",    "SetBkColor",         WP(winapi_set_bk_color)},
    {"gdi32.dll",    "GetBkColor",         WP(winapi_get_bk_color)},
    {"gdi32.dll",    "SetBkMode",          WP(winapi_set_bk_mode)},
    {"gdi32.dll",    "CreateCompatibleBitmap", WP(winapi_create_compatible_bitmap)},
    {"gdi32.dll",    "CreateDIBSection",   WP(winapi_create_dib_section)},
    {"gdi32.dll",    "BitBlt",             WP(winapi_bit_blt)},
    {"gdi32.dll",    "StretchBlt",         WP(winapi_stretch_blt)},
    {"gdi32.dll",    "IntersectClipRect",  WP(winapi_intersect_clip_rect)},
    {"gdi32.dll",    "SelectClipRgn",      WP(winapi_select_clip_rgn)},
    {"gdi32.dll",    "CreateRectRgn",      WP(winapi_create_rect_rgn)},
    {"gdi32.dll",    "SetViewportOrgEx",   WP(winapi_set_viewport_org_ex)},
    {"gdi32.dll",    "SetWindowOrgEx",     WP(winapi_set_window_org_ex)},
    {"gdi32.dll",    "GetViewportOrgEx",   WP(winapi_get_viewport_org_ex)},
    {"gdi32.dll",    "GetWindowOrgEx",     WP(winapi_get_window_org_ex)},
    {"gdi32.dll",    "SaveDC",             WP(winapi_save_dc)},
    {"gdi32.dll",    "RestoreDC",          WP(winapi_restore_dc)},
    {"gdi32.dll",    "RGB",                WP(winapi_rgb)},
    {"gdi32.dll",    "GetDeviceCaps",      WP(winapi_get_device_caps)},


    // kernel32.dll - File System API
    {"kernel32.dll", "CreateFileA",         WP(winapi_create_file_a)},
    {"kernel32.dll", "CreateFile",          WP(winapi_create_file_a)},
    {"kernel32.dll", "ReadFile",            WP(winapi_read_file)},
    {"kernel32.dll", "WriteFile",           WP(winapi_write_file)},
    {"kernel32.dll", "SetFilePointer",      WP(winapi_set_file_pointer)},
    {"kernel32.dll", "SetFilePointerEx",    WP(winapi_set_file_pointer)},
    {"kernel32.dll", "GetFileSize",         WP(winapi_get_file_size)},
    {"kernel32.dll", "DeleteFileA",         WP(winapi_delete_file_a)},
    {"kernel32.dll", "DeleteFile",          WP(winapi_delete_file_a)},
    {"kernel32.dll", "GetFileAttributesA",  WP(winapi_get_file_attributes_a)},
    {"kernel32.dll", "GetFileAttributes",   WP(winapi_get_file_attributes_a)},
    {"kernel32.dll", "SetFileAttributesA",  WP(winapi_set_file_attributes_a)},
    {"kernel32.dll", "SetFileAttributes",   WP(winapi_set_file_attributes_a)},
    {"kernel32.dll", "CreateDirectoryA",    WP(winapi_create_directory_a)},
    {"kernel32.dll", "CreateDirectory",     WP(winapi_create_directory_a)},
    {"kernel32.dll", "RemoveDirectoryA",    WP(winapi_remove_directory_a)},
    {"kernel32.dll", "RemoveDirectory",     WP(winapi_remove_directory_a)},
    {"kernel32.dll", "FindFirstFileA",      WP(winapi_find_first_file_a)},
    {"kernel32.dll", "FindFirstFile",       WP(winapi_find_first_file_a)},
    {"kernel32.dll", "FindNextFileA",       WP(winapi_find_next_file_a)},
    {"kernel32.dll", "FindNextFile",        WP(winapi_find_next_file_a)},
    {"kernel32.dll", "FindClose",           WP(winapi_find_close)},
    {"kernel32.dll", "GetCurrentDirectoryA", WP(winapi_get_current_directory_a)},
    {"kernel32.dll", "GetCurrentDirectory",  WP(winapi_get_current_directory_a)},
    {"kernel32.dll", "SetCurrentDirectoryA", WP(winapi_set_current_directory_a)},
    {"kernel32.dll", "SetCurrentDirectory",  WP(winapi_set_current_directory_a)},
    {"kernel32.dll", "GetTempPathA",        WP(winapi_get_temp_path_a)},
    {"kernel32.dll", "GetTempPath",         WP(winapi_get_temp_path_a)},
    {"kernel32.dll", "GetModuleFileNameA",  WP(winapi_get_module_file_name_a)},
    {"kernel32.dll", "GetModuleFileName",   WP(winapi_get_module_file_name_a)},
    {"kernel32.dll", "MoveFileA",           WP(winapi_move_file_a)},
    {"kernel32.dll", "MoveFile",            WP(winapi_move_file_a)},
    {"kernel32.dll", "CopyFileA",           WP(winapi_copy_file_a)},
    {"kernel32.dll", "CopyFile",            WP(winapi_copy_file_a)},


    // Qt DLLs (minimal stubs for Qt5/Qt6 imports)
    // These map Qt's internal DLL names to C++ implementations
    // that delegate to our qt.c framework
    {"Qt5Core.dll",    "qRegisterMetaType",     WP(0)},
    {"Qt5Core.dll",    "QMetaType::registerType",WP(0)},
    {"Qt5Core.dll",    "QObject::connect",       WP(0)},
    {"Qt5Core.dll",    "QObject::disconnect",    WP(0)},
    {"Qt5Core.dll",    "QCoreApplication::exec", WP(0)},
    {"Qt5Core.dll",    "QTimer::start",          WP(0)},
    {"Qt5Core.dll",    "QVariant::toString",     WP(0)},
    {"Qt5Core.dll",    "QVariant::toInt",        WP(0)},
    {"Qt5Core.dll",    "QByteArray::append",     WP(0)},
    {"Qt5Core.dll",    "QList::append",          WP(0)},
    {"Qt5Core.dll",    "QMap::insert",           WP(0)},
    {"Qt5Core.dll",    "QMap::value",            WP(0)},
    {"Qt5Gui.dll",     "QPainter::begin",        WP(0)},
    {"Qt5Gui.dll",     "QPainter::drawText",     WP(0)},
    {"Qt5Gui.dll",     "QPainter::drawPixmap",   WP(0)},
    {"Qt5Gui.dll",     "QPainter::fillRect",     WP(0)},
    {"Qt5Gui.dll",     "QFont::setPointSize",    WP(0)},
    {"Qt5Gui.dll",     "QPixmap::load",          WP(0)},
    {"Qt5Widgets.dll", "QApplication::exec",     WP(0)},
    {"Qt5Widgets.dll", "QWidget::show",          WP(0)},
    {"Qt5Widgets.dll", "QWidget::hide",          WP(0)},
    {"Qt5Widgets.dll", "QWidget::close",         WP(0)},
    {"Qt5Widgets.dll", "QWidget::resize",        WP(0)},
    {"Qt5Widgets.dll", "QWidget::setWindowTitle",WP(0)},
    {"Qt5Widgets.dll", "QLabel::setText",        WP(0)},
    {"Qt5Widgets.dll", "QPushButton::setText",   WP(0)},
    {"Qt5Widgets.dll", "QLineEdit::setText",     WP(0)},
    {"Qt5Widgets.dll", "QTextEdit::setText",     WP(0)},
    {"Qt5Widgets.dll", "QTextEdit::append",      WP(0)},
    {"Qt5Widgets.dll", "QScrollArea::setWidget", WP(0)},
    {"Qt5Network.dll", "QNetworkAccessManager::get",WP(0)},
    {"Qt5Network.dll", "QNetworkAccessManager::post",WP(0)},
    {"Qt5Network.dll", "QNetworkReply::readAll", WP(0)},

    // Sentinel
    {NULL, NULL, 0}
};
// ─── user32.dll windowing stubs ────────────────────────────────
HWND winapi_create_window_ex_a(DWORD style, LPCSTR cls, LPCSTR title,
    DWORD wstyle, int x, int y, int w, int h, HWND parent, void* menu,
    void* inst, void* lp) {
    (void)style;(void)cls;(void)title;(void)wstyle;(void)x;(void)y;
    (void)w;(void)h;(void)parent;(void)menu;(void)inst;(void)lp;
    return (HWND)0x10001;
}
BOOL winapi_destroy_window(HWND h) { (void)h; return TRUE; }
LRESULT winapi_def_window_proc_a(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h;(void)m;(void)w;(void)l; return 0;
}
BOOL winapi_show_window(HWND hWnd, int cmd) { (void)hWnd;(void)cmd; return TRUE; }
BOOL winapi_update_window(HWND hWnd) { (void)hWnd; return TRUE; }
BOOL winapi_move_window(HWND hWnd, int x, int y, int w, int ht, BOOL r) {
    (void)hWnd;(void)x;(void)y;(void)w;(void)ht;(void)r; return TRUE;
}
BOOL winapi_set_window_pos(HWND hWnd, HWND a, int x, int y, int w, int ht, UINT f) {
    (void)hWnd;(void)a;(void)x;(void)y;(void)w;(void)ht;(void)f; return TRUE;
}
BOOL winapi_set_window_text_a(HWND hWnd, LPCSTR t) { (void)hWnd;(void)t; return TRUE; }
int winapi_get_window_text_a(HWND hWnd, LPSTR b, int n) {
    (void)hWnd; if(b&&n>0) b[0]=0; return 0;
}
BOOL winapi_set_window_long_a(HWND hWnd, int i, LONG v) { (void)hWnd;(void)i;(void)v; return 0; }
LONG winapi_get_window_long_a(HWND hWnd, int i) { (void)hWnd;(void)i; return 0; }
BOOL winapi_get_window_rect(HWND hWnd, RECT* r) { (void)hWnd;(void)r; return FALSE; }
BOOL winapi_get_client_rect(HWND hWnd, RECT* r) { (void)hWnd;(void)r; return FALSE; }
BOOL winapi_invalidate_rect(HWND hWnd, const RECT* r, BOOL e) { (void)hWnd;(void)r;(void)e; return TRUE; }
BOOL winapi_validate_rect(HWND hWnd, const RECT* r) { (void)hWnd;(void)r; return TRUE; }
BOOL winapi_screen_to_client(HWND h, void* p) { (void)h;(void)p; return TRUE; }
BOOL winapi_client_to_screen(HWND h, void* p) { (void)h;(void)p; return TRUE; }
BOOL winapi_set_cursor_pos(int x, int y) { (void)x;(void)y; return TRUE; }
BOOL winapi_get_cursor_pos(void* p) { (void)p; return TRUE; }
HCURSOR winapi_set_cursor(HCURSOR h) { (void)h; return NULL; }
BOOL winapi_show_cursor(BOOL s) { (void)s; return TRUE; }
HWND winapi_set_capture(HWND h) { (void)h; return NULL; }
BOOL winapi_get_capture(void) { return FALSE; }
BOOL winapi_release_capture(void) { return TRUE; }
SHORT winapi_get_key_state(int k) { (void)k; return 0; }
SHORT winapi_get_async_key_state(int k) { (void)k; return 0; }
UINT winapi_get_key_name_text_a(LONG l, LPSTR b, int n) {
    (void)l; if(b&&n>0) b[0]=0; return 0;
}
BOOL winapi_translate_message(const void* m) { (void)m; return FALSE; }
LONG winapi_dispatch_message_a(const void* m) { (void)m; return FALSE; }
BOOL winapi_get_message_a(void* m, HWND h, UINT fMin, UINT fMax) {
    (void)m;(void)h;(void)fMin;(void)fMax; return FALSE; }
BOOL winapi_peek_message_a(void* m, HWND h, UINT fMin, UINT fMax, UINT rem) {
    (void)m;(void)h;(void)fMin;(void)fMax;(void)rem; return FALSE; }
BOOL winapi_post_message_a(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h;(void)m;(void)w;(void)l; return FALSE; }
void winapi_post_quit_message(int c) { (void)c; }
BOOL winapi_send_message_a(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h;(void)m;(void)w;(void)l; return 0; }
HDC winapi_begin_paint(HWND hWnd, void* ps) { (void)hWnd;(void)ps; return NULL; }
int winapi_end_paint(HWND hWnd, const void* ps) { (void)hWnd;(void)ps; return TRUE; }
HMENU winapi_create_menu(void) { return (HMENU)0x10002; }
HMENU winapi_create_popup_menu(void) { return (HMENU)0x10003; }
BOOL winapi_append_menu_a(HMENU h, UINT f, UINT_PTR id, LPCSTR s) {
    (void)h;(void)f;(void)id;(void)s; return TRUE; }
BOOL winapi_destroy_menu(HMENU h) { (void)h; return TRUE; }
int winapi_trackPopupMenu(HMENU hMenu, UINT fuFlags, int x, int y, int nReserved, HWND hWnd, const void* prcRect) {
    (void)hMenu;(void)fuFlags;(void)x;(void)y;(void)nReserved;(void)hWnd;(void)prcRect; return TRUE; }


const int winapi_table_size = (sizeof(winapi_table) / sizeof(winapi_table[0])) - 1;

// Look up a function by DLL name + function name.
uint32_t winapi_lookup(const char* dll_name, const char* func_name) {
    if (!dll_name || !func_name) return 0;
    for (int i = 0; i < winapi_table_size; i++) {
        if (str_eq_local(winapi_table[i].dll_name, dll_name) &&
            str_eq_local(winapi_table[i].func_name, func_name)) {
            return winapi_table[i].impl_addr;
        }
    }
    return 0;
}
