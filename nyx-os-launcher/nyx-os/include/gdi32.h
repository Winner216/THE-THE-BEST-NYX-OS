// include/gdi32.h  –  GDI (Graphics Device Interface) compatibility for Nyx OS.
//
// Provides Win32 GDI function stubs so PE executables (like Telegram/Qt)
// can link and perform basic drawing operations.  Maps to Nyx OS's
// VESA framebuffer primitives.
//
// Supported:
//   - Device contexts (HDC): GetDC, ReleaseDC, CreateCompatibleDC, DeleteDC
//   - Drawing: MoveToEx, LineTo, Rectangle, Ellipse, Polygon
//   - Pens/Brushes: CreatePen, CreateSolidBrush, SelectObject, DeleteObject
//   - Text: TextOutA, DrawTextA, GetTextMetricsA, GetTextExtentPoint32A
//   - Bitmaps: CreateCompatibleBitmap, BitBlt, StretchBlt
//   - Clipping: IntersectClipRect, SelectClipRgn
//   - Mapping: SetViewportOrgEx, SetWindowOrgEx

#ifndef GDI32_H
#define GDI32_H

#include <stdint.h>

// ─── GDI types ──────────────────────────────────────────────────
typedef void* HDC;
typedef void* HBITMAP;
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HFONT;
typedef void* HGDIOBJ;
typedef void* HRGN;
typedef void* HPALETTE;
typedef void* HMONITOR;

typedef struct { long left, top, right, bottom; } RECT;
typedef struct { long x, y; } POINT;
typedef struct { uint32_t dwSize, dwFlags, dwID, dwErrorStatus, dwRequestStatus; } LUMMY;

// GDI constants
#define GDI_ERROR           ((int)0xFFFFFFFF)
#define INVALID_GDI_HANDLE  ((void*)0)

// Device capabilities
#define HORZRES             8
#define VERTRES             10
#define BITSPIXEL           12
#define PLANES              14

// Text alignment
#define TA_LEFT      0
#define TA_RIGHT     2
#define TA_CENTER    6
#define TA_TOP       0
#define TA_BASELINE  24

// Polyfill modes
#define ALTERNATE    1
#define WINDING      2

// Raster operations
#define SRCCOPY      0x00CC0020
#define SRCPAINT     0x00EE0086
#define SRCAND       0x008800C6
#define SRCINVERT    0x00660046

// GDI object types
#define OBJ_PEN      1
#define OBJ_BRUSH    2
#define OBJ_FONT     3
#define OBJ_BITMAP   4
#define OBJ_REGION   5

// ─── Device context functions ───────────────────────────────────

HDC GetDC(void* hwnd);
int ReleaseDC(void* hwnd, HDC hdc);
HDC CreateCompatibleDC(HDC hdc);
int DeleteDC(HDC hdc);
HDC GetWindowDC(void* hwnd);
HDC GetClientRectDC(void* hwnd);

// ─── Drawing functions ──────────────────────────────────────────

int MoveToEx(HDC hdc, int x, int y, void* lppt);
int LineTo(HDC hdc, int x, int y);
int Rectangle(HDC hdc, int left, int top, int right, int bottom);
int Ellipse(HDC hdc, int left, int top, int right, int bottom);
int Polygon(HDC hdc, const POINT* lppt, int cPoints);
int Polyline(HDC hdc, const POINT* lppt, int cPoints);
int Arc(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4);
int Pie(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4);
int Chord(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4);
int RoundRect(HDC hdc, int left, int top, int right, int bottom, int xWidth, int yHeight);

// ─── Pen/Brush functions ────────────────────────────────────────

HPEN CreatePen(int iStyle, int cWidth, uint32_t color);
HPEN CreatePenIndirect(const void* lplgpn);
HBRUSH CreateSolidBrush(uint32_t color);
HBRUSH CreateHatchBrush(int iStyle, uint32_t color);
HBRUSH CreatePatternBrush(HBITMAP hbm);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h);
int DeleteObject(HGDIOBJ ho);
int GetObject(HGDIOBJ h, int c, void* pv);
HGDIOBJ GetStockObject(int i);

// Stock objects
#define WHITE_BRUSH   0
#define LTGRAY_BRUSH  1
#define GRAY_BRUSH    2
#define DKGRAY_BRUSH  3
#define BLACK_BRUSH   4
#define WHITE_PEN     6
#define BLACK_PEN     7
#define NULL_PEN      8
#define DEFAULT_PEN   11
#define NULL_BRUSH    10
#define HOLLOW_BRUSH  NULL_BRUSH
#define DEFAULT_GUI_FONT 17

// ─── Text functions ─────────────────────────────────────────────

int TextOutA(HDC hdc, int x, int y, const char* lpString, int c);
int TextOutW(HDC hdc, int x, int y, const uint16_t* lpString, int c);
int DrawTextA(HDC hdc, const char* lpchText, int cchText, RECT* lprc, uint32_t format);
int DrawTextW(HDC hdc, const uint16_t* lpchText, int cchText, RECT* lprc, uint32_t format);
int GetTextMetricsA(HDC hdc, void* lptm);
int GetTextExtentPoint32A(HDC hdc, const char* lpString, int c, POINT* lpsz);
int SetTextAlign(HDC hdc, uint32_t fMode);
uint32_t GetTextAlign(HDC hdc);
int SetTextColor(HDC hdc, uint32_t color);
uint32_t GetTextColor(HDC hdc);
int SetBkColor(HDC hdc, uint32_t color);
uint32_t GetBkColor(HDC hdc);
int SetBkMode(HDC hdc, int iBkMode);

// Text modes
#define OPAQUE       2
#define TRANSPARENT  1

// DrawText formats
#define DT_LEFT        0x00000000
#define DT_TOP         0x00000000
#define DT_CENTER      0x00000001
#define DT_RIGHT       0x00000002
#define DT_VCENTER     0x00000004
#define DT_BOTTOM      0x00000008
#define DT_WORDBREAK   0x00000010
#define DT_SINGLELINE  0x00000020
#define DT_EXPANDTABS  0x00000040
#define DT_TABSTOP     0x00000080
#define DT_NOCLIP      0x00000100
#define DT_CALCRECT    0x00000400
#define DT_NOPREFIX    0x00000800
#define DT_INTERNAL    0x00001000

// ─── Bitmap functions ───────────────────────────────────────────

HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy);
HBITMAP CreateBitmap(int nWidth, int nHeight, uint32_t nPlanes, uint32_t nBitsPerPel, const void* lpBits);
HBITMAP CreateDIBSection(HDC hdc, const void* pbmi, uint32_t usage, void** ppvBits, void* hSection, uint32_t offset);
int BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, uint32_t rop);
int StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
               HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, uint32_t rop);

// ─── Clipping functions ─────────────────────────────────────────

int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom);
int SelectClipRgn(HDC hdc, HRGN hrgn);
HRGN CreateRectRgn(int x1, int y1, int x2, int y2);

// ─── Mapping functions ──────────────────────────────────────────

int SetViewportOrgEx(HDC hdc, int x, int y, POINT* lpPoint);
int SetWindowOrgEx(HDC hdc, int x, int y, POINT* lpPoint);
int GetViewportOrgEx(HDC hdc, POINT* lpPoint);
int GetWindowOrgEx(HDC hdc, POINT* lpPoint);

// ─── DC state ───────────────────────────────────────────────────

int SaveDC(HDC hdc);
int RestoreDC(HDC hdc, int nSavedDC);

// ─── Color functions ────────────────────────────────────────────

uint32_t RGB(uint8_t r, uint8_t g, uint8_t b);
uint8_t GetRValue(uint32_t rgb);
uint8_t GetGValue(uint32_t rgb);
uint8_t GetBValue(uint32_t rgb);

#endif
