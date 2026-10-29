// include/winapi.h  –  Win32 API compatibility layer for Nyx OS.
//
// Provides stub/minimal implementations of common Windows API functions
// so PE executables can link and run basic operations.  The import
// resolver in peload.c maps DLL function names to these implementations.
//
// Architecture:
//   1. PE imports a function (e.g. WriteConsoleA from kernel32.dll)
//   2. peload.c resolves it to winapi_write_console() via the lookup table
//   3. The PE calls the function; our implementation translates it to
//      Nyx OS kernel calls (terminal output, memory alloc, etc.)
//
// Currently supported:
//   - kernel32.dll:  ExitProcess, GetStdHandle, WriteConsoleA,
//                    Sleep, GetTickCount, HeapAlloc, HeapFree,
//                    GetModuleHandleA, GetProcAddress,
//                    VirtualAlloc, VirtualFree, GetCommandLineA
//   - user32.dll:    MessageBoxA (stub — prints to serial)
//   - msvcrt.dll:    printf, puts, malloc, free, exit (mapped to kernel)
//
// This is NOT a full Win32 implementation — it's enough to run simple
// console programs and demonstrates the translation architecture.

#ifndef WINAPI_H
#define WINAPI_H

#include <stdint.h>

// ─── Win32 type definitions ─────────────────────────────────────
// These match the Windows SDK types so PE code compiles without changes.

typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned long       DWORD;
typedef long                LONG;
typedef unsigned long       ULONG;
typedef void*               HANDLE;
typedef void*               HMODULE;
typedef void*               HINSTANCE;
typedef void*               HLOCAL;
typedef void*               LPVOID;
typedef const char*         LPCSTR;
typedef const uint16_t*     LPCWSTR;
typedef char*               LPSTR;
typedef uint16_t*           LPWSTR;
typedef unsigned long       SIZE_T;
typedef unsigned long       UINT;
typedef DWORD*              LPDWORD;
typedef long                LRESULT;
typedef unsigned long       WPARAM;
typedef long                LPARAM;
typedef void*               HWND;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

// Standard handles
#define STD_INPUT_HANDLE   ((DWORD)-10)
#define STD_OUTPUT_HANDLE  ((DWORD)-11)
#define STD_ERROR_HANDLE   ((DWORD)-12)

// Memory allocation flags
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_FREE     0x10000
#define MEM_PRIVATE  0x20000
#define PAGE_READWRITE 0x04

// MessageBox flags
#define MB_OK               0x00000000
#define MB_OKCANCEL         0x00000001
#define MB_YESNO            0x00000004
#define MB_ICONERROR        0x00000010
#define MB_ICONWARNING      0x00000030
#define MB_ICONINFORMATION  0x00000040

#define IDOK     1
#define IDCANCEL 2
#define IDYES    6
#define IDNO     7

// ─── Win32 API function prototypes ──────────────────────────────
// Each function maps to a winapi_* implementation in winapi.c.

// kernel32.dll
void     winapi_exit_process(UINT uExitCode);
DWORD    winapi_get_std_handle(DWORD nStdHandle);
BOOL     winapi_write_console_a(HANDLE hConsoleOutput, const void* lpBuffer,
                                DWORD nNumberOfCharsToWrite,
                                LPDWORD lpNumberOfCharsWritten,
                                void* lpReserved);
void     winapi_sleep(DWORD dwMilliseconds);
DWORD    winapi_get_tick_count(void);
LPVOID   winapi_heap_alloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
BOOL     winapi_heap_free(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
HMODULE  winapi_get_module_handle_a(LPCSTR lpModuleName);
void*    winapi_get_proc_address(HMODULE hModule, LPCSTR lpProcName);
LPVOID   winapi_virtual_alloc(LPVOID lpAddress, SIZE_T dwSize,
                              DWORD flAllocationType, DWORD flProtect);
BOOL     winapi_virtual_free(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
LPSTR    winapi_get_command_line_a(void);

// Threading constants
#define WAIT_OBJECT_0    0
#define WAIT_TIMEOUT     0x102
#define WAIT_FAILED      ((DWORD)0xFFFFFFFF)
#define INFINITE         ((DWORD)0xFFFFFFFF)

// Thread creation flags
#define CREATE_SUSPENDED  0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION  0x00010000

// user32.dll
int      winapi_message_box_a(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);

// msvcrt.dll (C runtime mapped to kernel)
int      winapi_printf(const char* format, ...);
int      winapi_puts(const char* s);
void*    winapi_malloc(SIZE_T size);
void     winapi_free(void* ptr);
void     winapi_exit(int status);

// ─── Threading & Synchronization APIs ──────────────────────────
// These map kernel32.dll functions to Nyx OS task/sync primitives.

// Thread management
HANDLE winapi_create_thread(void* lpThreadAttributes, SIZE_T dwStackSize,
                            void* lpStartAddress, void* lpParameter,
                            DWORD dwCreationFlags, DWORD* lpThreadId);
DWORD  winapi_get_current_thread_id(void);
void   winapi_exit_thread(DWORD dwExitCode);
DWORD  winapi_wait_for_single_object(HANDLE hHandle, DWORD dwMilliseconds);
DWORD  winapi_wait_for_multiple_objects(DWORD nCount, const HANDLE* lpHandles,
                                       BOOL bWaitAll, DWORD dwMilliseconds);
BOOL   winapi_close_handle(HANDLE hObject);

// Mutex
HANDLE winapi_create_mutex_a(void* lpMutexAttributes, BOOL bInitialOwner,
                             LPCSTR lpName);
BOOL   winapi_release_mutex(HANDLE hMutex);

// Semaphore
HANDLE winapi_create_semaphore_a(void* lpSemaphoreAttributes,
                                 LONG lInitialCount, LONG lMaximumCount,
                                 LPCSTR lpName);
BOOL   winapi_release_semaphore(HANDLE hSemaphore, LONG lReleaseCount,
                                LONG* lpPreviousCount);

// Critical Section
void   winapi_initialize_critical_section(void* lpCriticalSection);
void   winapi_enter_critical_section(void* lpCriticalSection);
BOOL   winapi_try_enter_critical_section(void* lpCriticalSection);
void   winapi_leave_critical_section(void* lpCriticalSection);
void   winapi_delete_critical_section(void* lpCriticalSection);

// Event
HANDLE winapi_create_event_a(void* lpEventAttributes, BOOL bManualReset,
                             BOOL bInitialState, LPCSTR lpName);
BOOL   winapi_set_event(HANDLE hEvent);
BOOL   winapi_reset_event(HANDLE hEvent);

// SRWLOCK (slim reader-writer lock) — lightweight sync
void   winapi_initialize_srw_lock(void* SRWLock);
void   winapi_acquire_srw_lock_exclusive(void* SRWLock);
void   winapi_release_srw_lock_exclusive(void* SRWLock);
void   winapi_acquire_srw_lock_shared(void* SRWLock);
void   winapi_release_srw_lock_shared(void* SRWLock);

// Condition variable
void   winapi_initialize_condition_variable(void* lpConditionVariable);
BOOL   winapi_sleep_condition_variable_cs(void* lpConditionVariable,
                                          void* lpCriticalSection,
                                          DWORD dwMilliseconds);
BOOL   winapi_sleep_condition_variable_srw(void* lpConditionVariable,
                                           void* SRWLock,
                                           DWORD dwMilliseconds);
void   winapi_wake_condition_variable(void* lpConditionVariable);
void   winapi_wake_all_condition_variable(void* lpConditionVariable);

// Interlocked operations (atomic)
LONG   winapi_interlocked_exchange(volatile LONG* Target, LONG Value);
LONG   winapi_interlocked_compare_exchange(volatile LONG* Destination,
                                           LONG ExChange, LONG Comperand);
LONG   winapi_interlocked_increment(volatile LONG* lpAddend);
LONG   winapi_interlocked_decrement(volatile LONG* lpAddend);
LONG   winapi_interlocked_add(volatile LONG* lpAddend, LONG Value);

// Timed wait
DWORD  winapi_wait_on_address(void* Address, void* CompareAddress,
                              SIZE_T AddressSize, DWORD dwMilliseconds);
void   winapi_wake_by_address_single(void* Address);
void   winapi_wake_by_address_all(void* Address);

// Thread-local storage
DWORD  winapi_tls_alloc(void);
BOOL   winapi_tls_free(DWORD dwTlsIndex);
void*  winapi_tls_get_value(DWORD dwTlsIndex);
BOOL   winapi_tls_set_value(DWORD dwTlsIndex, void* lpTlsValue);

// ─── Additional Win32 types needed by user32/gdi32 ──────────────
typedef void* HDC;
typedef void* HBITMAP;
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HFONT;
typedef void* HGDIOBJ;
typedef void* HRGN;
typedef void* HPALETTE;
typedef void* HMONITOR;
typedef void* HCURSOR;
typedef void* HICON;
typedef void* HMENU;

typedef int SHORT;
typedef long LONG_PTR;
typedef unsigned long ULONG_PTR;
typedef ULONG_PTR UINT_PTR;

#ifndef POINT
typedef struct { long x, y; } POINT;
#endif

#ifndef RECT_DEFINED
#define RECT_DEFINED
typedef struct { long left, top, right, bottom; } RECT;
#endif

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// ─── user32.dll Windowing API ─────────────────────────────────

// Window creation and management
HWND    winapi_create_window_ex_a(DWORD dwExStyle, LPCSTR lpClassName,
                                   LPCSTR lpWindowName, DWORD dwStyle,
                                   int x, int y, int nWidth, int nHeight,
                                   HWND hWndParent, void* hMenu,
                                   HINSTANCE hInstance, void* lpParam);
BOOL    winapi_destroy_window(HWND hWnd);
BOOL    winapi_show_window(HWND hWnd, int nCmdShow);
BOOL    winapi_update_window(HWND hWnd);
BOOL    winapi_set_window_pos(HWND hWnd, HWND hWndInsertAfter,
                               int X, int Y, int cx, int cy, UINT uFlags);
BOOL    winapi_move_window(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL    winapi_set_window_text_a(HWND hWnd, LPCSTR lpString);
int     winapi_get_window_text_a(HWND hWnd, LPSTR lpString, int nMaxCount);
BOOL    winapi_get_window_rect(HWND hWnd, RECT* lpRect);
BOOL    winapi_get_client_rect(HWND hWnd, RECT* lpRect);
BOOL    winapi_set_window_long_a(HWND hWnd, int nIndex, LONG dwNewLong);
LONG    winapi_get_window_long_a(HWND hWnd, int nIndex);

// Message loop
BOOL    winapi_get_message_a(void* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL    winapi_peek_message_a(void* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL    winapi_translate_message(const void* lpMsg);
LONG    winapi_dispatch_message_a(const void* lpMsg);
BOOL    winapi_post_message_a(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL    winapi_send_message_a(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
void    winapi_post_quit_message(int nExitCode);
LRESULT winapi_def_window_proc_a(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

// Mouse
BOOL    winapi_get_cursor_pos(void* lpPoint);
BOOL    winapi_set_cursor_pos(int X, int Y);
HCURSOR winapi_set_cursor(HCURSOR hCursor);
int     winapi_show_cursor(BOOL bShow);
BOOL    winapi_get_capture(void);
HWND    winapi_set_capture(HWND hWnd);
BOOL    winapi_release_capture(void);
int     winapi_client_to_screen(HWND hWnd, void* lpPoint);
int     winapi_screen_to_client(HWND hWnd, void* lpPoint);

// Keyboard
SHORT   winapi_get_async_key_state(int vKey);
SHORT   winapi_get_key_state(int nVirtKey);
UINT    winapi_get_key_name_text_a(LONG lParam, LPSTR lpString, int cchSize);

// Drawing surface / paint
HDC     winapi_get_dc(HWND hWnd);
HDC     winapi_get_window_dc(HWND hWnd);
int     winapi_release_dc(HWND hWnd, HDC hDC);
HDC     winapi_begin_paint(HWND hWnd, void* lpPaint);
int     winapi_end_paint(HWND hWnd, const void* lpPaint);
BOOL    winapi_invalidate_rect(HWND hWnd, const RECT* lpRect, BOOL bErase);
BOOL    winapi_validate_rect(HWND hWnd, const RECT* lpRect);

// Menu
HMENU   winapi_create_menu(void);
HMENU   winapi_create_popup_menu(void);
BOOL    winapi_append_menu_a(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem);
BOOL    winapi_destroy_menu(HMENU hMenu);
int     winapi_trackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const void* prcRect);

// Timer
UINT    winapi_set_timer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, void* lpTimerFunc);
BOOL    winapi_kill_timer(HWND hWnd, UINT_PTR uIDEvent);

// Clipboard
BOOL    winapi_open_clipboard(HWND hWndNewOwner);
BOOL    winapi_close_clipboard(void);
HANDLE  winapi_get_clipboard_data(UINT uFormat);
HANDLE  winapi_set_clipboard_data(UINT uFormat, HANDLE hMem);
UINT    winapi_register_clipboard_format_a(LPCSTR lpszFormat);

// Dialog / Message Box (already have MessageBoxA)
int     winapi_message_box_a(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);

// Window class registration
UINT    winapi_register_class_a(const void* lpWndClass);
BOOL    winapi_unregister_class_a(LPCSTR lpClassName, HINSTANCE hInstance);

// Focus / Activation
HWND    winapi_set_focus(HWND hWnd);
HWND    winapi_get_focus(void);
HWND    winapi_get_active_window(void);
HWND    winapi_set_active_window(HWND hWnd);
HWND    winapi_get_foreground_window(void);
BOOL    winapi_set_foreground_window(HWND hWnd);

// Misc
HINSTANCE winapi_get_module_handle_a(LPCSTR lpModuleName);
int       winapi_get_system_metrics(int nIndex);
BOOL      winapi_system_parameters_info_a(UINT uiAction, UINT uiParam, void* pvParam, UINT fWinIni);

// System metrics constants
#define SM_CXSCREEN     0
#define SM_CYSCREEN     1
#define SM_CXVSCROLL    2
#define SM_CYHSCROLL    3
#define SM_CYCAPTION    4
#define SM_CXBORDER     5
#define SM_CYBORDER     6
#define SM_CXFIXEDFRAME 7
#define SM_CYFIXEDFRAME 8

// Window styles
#define WS_OVERLAPPED       0x00000000
#define WS_POPUP            0x80000000
#define WS_CHILD            0x40000000
#define WS_VISIBLE          0x10000000
#define WS_DISABLED         0x08000000
#define WS_CLIPSIBLINGS     0x04000000
#define WS_CLIPCHILDREN     0x02000000
#define WS_MAXIMIZE         0x01000000
#define WS_CAPTION          0x00C00000
#define WS_BORDER           0x00800000
#define WS_DLGFRAME         0x00400000
#define WS_VSCROLL          0x00200000
#define WS_HSCROLL          0x00100000
#define WS_SYSMENU          0x00080000
#define WS_THICKFRAME       0x00040000
#define WS_MINIMIZEBOX      0x00020000
#define WS_MAXIMIZEBOX      0x00010000
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX)

// Extended window styles
#define WS_EX_CLIENTEDGE    0x00000200
#define WS_EX_WINDOWEDGE    0x00000100
#define WS_EX_TOPMOST       0x00000008
#define WS_EX_TOOLWINDOW    0x00000080
#define WS_EX_APPWINDOW     0x00040000

// ShowWindow commands
#define SW_HIDE             0
#define SW_SHOW             5
#define SW_MINIMIZE         6
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_RESTORE          9

// Window messages
#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_ERASEBKGND       0x0014
#define WM_SHOWWINDOW       0x0018
#define WM_ACTIVATEAPP      0x001C
#define WM_SETCURSOR        0x0020
#define WM_MOUSEACTIVATE    0x0021
#define WM_GETMINMAXINFO    0x0024
#define WM_PAINTICON        0x0026
#define WM_ICONERASEBKGND   0x0027
#define WM_NEXTDLGCTL       0x0028
#define WM_SPOOLERSTATUS    0x002A
#define WM_DRAWITEM         0x002B
#define WM_MEASUREITEM      0x002C
#define WM_DELETEITEM       0x002D
#define WM_VKEYTOITEM       0x002E
#define WM_CHARTOITEM       0x002F
#define WM_SETFONT          0x0030
#define WM_GETFONT          0x0031
#define WM_SETHOTKEY        0x0032
#define WM_GETHOTKEY        0x0033
#define WM_QUERYDRAGICON    0x0037
#define WM_COMPAREITEM      0x0039
#define WM_COMPACTING       0x0041
#define WM_COMMNOTIFY       0x0044
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_POWER            0x0048
#define WM_COPYDATA         0x004A
#define WM_CANCELJOURNAL    0x004B
#define WM_INPUTLANGCHANGEREQUEST 0x0050
#define WM_INPUTLANGCHANGE  0x0051
#define WM_TCARD            0x0052
#define WM_HELP             0x0053
#define WM_USERCHANGED      0x0054
#define WM_NOTIFYFORMAT     0x0055
#define WM_CONTEXTMENU      0x007B
#define WM_STYLECHANGING    0x007C
#define WM_STYLECHANGED     0x007D
#define WM_DISPLAYCHANGE    0x007E
#define WM_GETICON          0x007F
#define WM_SETICON          0x0080
#define WM_NCCREATE         0x0081
#define WM_NCDESTROY        0x0082
#define WM_NCCALCSIZE       0x0083
#define WM_NCHITTEST        0x0084
#define WM_NCPAINT          0x0085
#define WM_NCACTIVATE       0x0086
#define WM_GETDLGCODE       0x0087
#define WM_NCMOUSEMOVE      0x00A0
#define WM_NCLBUTTONDOWN    0x00A1
#define WM_NCLBUTTONUP      0x00A2
#define WM_NCLBUTTONDBLCLK  0x00A3
#define WM_NCRBUTTONDOWN    0x00A4
#define WM_NCRBUTTONUP      0x00A5
#define WM_NCRBUTTONDBLCLK  0x00A6
#define WM_NCMBUTTONDOWN    0x00A7
#define WM_NCMBUTTONUP      0x00A8
#define WM_NCMBUTTONDBLCLK  0x00A9
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_DEADCHAR         0x0103
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_SYSCHAR          0x0106
#define WM_SYSDEADCHAR      0x0107
#define WM_UNICHAR          0x0109
#define WM_IME_STARTCOMPOSITION  0x010D
#define WM_IME_ENDCOMPOSITION    0x010E
#define WM_IME_COMPOSITION      0x010F
#define WM_IME_KEYLAST          0x010F
#define WM_INITDIALOG      0x0110
#define WM_COMMAND          0x0111
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_HSCROLL          0x0114
#define WM_VSCROLL          0x0115
#define WM_INITMENU         0x0116
#define WM_INITMENUPOPUP    0x0117
#define WM_MENUSELECT       0x011F
#define WM_MENUCHAR         0x0120
#define WM_ENTERIDLE        0x0121
#define WM_CTLCOLORMSGBOX   0x0132
#define WM_CTLCOLOREDIT     0x0133
#define WM_CTLCOLORLISTBOX  0x0134
#define WM_CTLCOLORBTN      0x0135
#define WM_CTLCOLORDLG      0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC   0x0138
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_RBUTTONDBLCLK    0x0206
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MBUTTONDBLCLK    0x0209
#define WM_MOUSEWHEEL       0x020A
#define WM_MOUSEHOVER       0x02A1
#define WM_MOUSELEAVE       0x02A3
#define WM_CUT              0x0300
#define WM_COPY             0x0301
#define WM_PASTE            0x0302
#define WM_CLEAR            0x0303
#define WM_UNDO             0x0304
#define WM_RENDERFORMAT     0x0305
#define WM_RENDERALLFORMATS 0x0306
#define WM_DESTROYCLIPBOARD 0x0307
#define WM_DRAWCLIPBOARD    0x0308
#define WM_PAINTCLIPBOARD   0x0309
#define WM_VSCROLLCLIPBOARD 0x030A
#define WM_SIZECLIPBOARD    0x030B
#define WM_ASKCBFORMATNAME  0x030C
#define WM_CHANGECBCHAIN    0x030D
#define WM_HSCROLLCLIPBOARD 0x030E
#define WM_QUERYNEWPALETTE  0x030F
#define WM_PALETTEISCHANGING 0x0310
#define WM_PALETTECHANGED   0x0311
#define WM_HOTKEY           0x0312
#define WM_PRINT            0x0317
#define WM_PRINTCLIENT      0x0318
#define WM_HANDHELDFIRST    0x0358
#define WM_HANDHELDLAST     0x035F
#define WM_AFXFIRST         0x0360
#define WM_AFXLAST          0x037F
#define WM_PENWINFIRST      0x0380
#define WM_PENWINLAST       0x038F
#define WM_USER             0x0400
#define WM_APP              0x8000

// Virtual key codes
#define VK_LBUTTON     0x01
#define VK_RBUTTON     0x02
#define VK_MBUTTON     0x04
#define VK_BACK        0x08
#define VK_TAB         0x09
#define VK_RETURN      0x0D
#define VK_SHIFT       0x10
#define VK_CONTROL     0x11
#define VK_MENU        0x12
#define VK_PAUSE       0x13
#define VK_CAPITAL     0x14
#define VK_ESCAPE      0x1B
#define VK_SPACE       0x20
#define VK_PRIOR       0x21
#define VK_NEXT        0x22
#define VK_END         0x23
#define VK_HOME        0x24
#define VK_LEFT        0x25
#define VK_UP          0x26
#define VK_RIGHT       0x27
#define VK_DOWN        0x28
#define VK_INSERT      0x2D
#define VK_DELETE      0x2E
#define VK_0           0x30
#define VK_9           0x39
#define VK_A           0x41
#define VK_Z           0x5A
#define VK_LWIN        0x5B
#define VK_RWIN        0x5C
#define VK_APPS        0x5D
#define VK_NUMPAD0     0x60
#define VK_NUMPAD9     0x69
#define VK_F1          0x70
#define VK_F12         0x7B



// MSG structure
typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG;

// WNDCLASS structure
typedef struct tagWNDCLASSA {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
} WNDCLASSA;

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// ─── Import lookup table ────────────────────────────────────────
// Maps (DLL name, function name) → implementation pointer.
// The import resolver in peload.c uses this to fill the IAT.

typedef struct {
    const char* dll_name;
    const char* func_name;
    uint32_t    impl_addr;   // function pointer (cast to uint32_t)
} winapi_entry_t;

// The lookup table (defined in winapi.c)
extern const winapi_entry_t winapi_table[];
extern const int winapi_table_size;

// Look up a function by DLL name + function name.
// Returns the implementation address, or 0 if not found.
uint32_t winapi_lookup(const char* dll_name, const char* func_name);

#endif
