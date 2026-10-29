// cpu/gdi32.c  –  GDI compatibility for Nyx OS.
//
// Maps Win32 GDI drawing functions to Nyx OS's VESA framebuffer
// primitives.  Provides enough surface area for Telegram/Qt to
// link and perform basic drawing operations.
#include <gdi32.h>
#include <vesa.h>
#include <heap.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// ─── Internal DC structure ──────────────────────────────────────
typedef struct {
    int      in_use;
    uint32_t fg_color;        // current text/drawing color
    uint32_t bg_color;        // current background color
    int      bg_mode;         // OPAQUE or TRANSPARENT
    int      text_align;      // TA_LEFT | TA_CENTER | TA_RIGHT | TA_BASELINE
    int      pen_style;
    int      pen_width;
    uint32_t pen_color;
    uint32_t brush_color;
    int      brush_style;
    int      vx, vy;          // viewport origin
    int      wx, wy;          // window origin
    int      saved;           // nesting level
    HGDIOBJ  cur_pen;
    HGDIOBJ  cur_brush;
    HGDIOBJ  cur_font;
} gdi_dc_t;

#define MAX_DC 16
static gdi_dc_t dc_table[MAX_DC];

// ─── DC management ──────────────────────────────────────────────

static int alloc_dc(void) {
    for (int i = 0; i < MAX_DC; i++) {
        if (!dc_table[i].in_use) {
            memset(&dc_table[i], 0, sizeof(gdi_dc_t));
            dc_table[i].in_use = 1;
            dc_table[i].fg_color = 0x00000000;  // black
            dc_table[i].bg_color = 0x00FFFFFF;  // white
            dc_table[i].bg_mode = OPAQUE;
            dc_table[i].pen_color = 0x00000000;
            dc_table[i].brush_color = 0x00FFFFFF;
            dc_table[i].pen_width = 1;
            return i;
        }
    }
    return -1;
}

HDC GetDC(void* hwnd) {
    (void)hwnd;
    int idx = alloc_dc();
    if (idx < 0) return NULL;
    return (HDC)(intptr_t)(idx + 1);
}

int ReleaseDC(void* hwnd, HDC hdc) {
    (void)hwnd;
    if (!hdc) return 0;
    int idx = (int)(intptr_t)hdc - 1;
    if (idx >= 0 && idx < MAX_DC) dc_table[idx].in_use = 0;
    return 1;
}

HDC CreateCompatibleDC(HDC hdc) {
    (void)hdc;
    return GetDC(NULL);
}

int DeleteDC(HDC hdc) {
    return ReleaseDC(NULL, hdc);
}

HDC GetWindowDC(void* hwnd) { return GetDC(hwnd); }
HDC GetClientRectDC(void* hwnd) { return GetDC(hwnd); }

// ─── Helper: map to VESA colors ─────────────────────────────────

static gdi_dc_t* get_dc(HDC hdc) {
    if (!hdc) return NULL;
    int idx = (int)(intptr_t)hdc - 1;
    if (idx < 0 || idx >= MAX_DC || !dc_table[idx].in_use) return NULL;
    return &dc_table[idx];
}

// ─── Drawing functions ──────────────────────────────────────────

int MoveToEx(HDC hdc, int x, int y, void* lppt) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    // Store current position in a static (simplified)
    static int cur_x = 0, cur_y = 0;
    if (lppt) { ((POINT*)lppt)->x = cur_x; ((POINT*)lppt)->y = cur_y; }
    cur_x = x; cur_y = y;
    return 1;
}

int LineTo(HDC hdc, int x, int y) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    // Simplified: draw line from (0,0) to (x,y) using Bresenham
    static int from_x = 0, from_y = 0;
    int x0 = from_x, y0 = from_y;
    from_x = x; from_y = y;
    
    int dx = (x > x0) ? (x - x0) : (x0 - x);
    int dy = (y > y0) ? (y - y0) : (y0 - y);
    int sx = (x0 < x) ? 1 : -1;
    int sy = (y0 < y) ? 1 : -1;
    int err = dx - dy;
    
    for (;;) {
        vesa_put_pixel(x0, y0, dc->pen_color);
        if (x0 == x && y0 == y) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
    return 1;
}

int Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    // Fill with brush color
    vesa_fill_rect(left, top, right - left, bottom - top, dc->brush_color);
    // Draw border with pen color
    vesa_draw_rect(left, top, right - left, bottom - top, dc->pen_color);
    return 1;
}

int Ellipse(HDC hdc, int left, int top, int right, int bottom) {
    (void)hdc;
    // Simplified: draw filled rectangle as placeholder
    vesa_fill_rect(left, top, right - left, bottom - top, 0x00808080);
    return 1;
}

int Polygon(HDC hdc, const POINT* lppt, int cPoints) {
    (void)hdc; (void)lppt; (void)cPoints;
    return 1;  // stub
}

int Polyline(HDC hdc, const POINT* lppt, int cPoints) {
    (void)hdc; (void)lppt; (void)cPoints;
    return 1;  // stub
}

int Arc(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4) {
    (void)hdc; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)x4; (void)y4;
    return 1;
}

int Pie(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4) {
    (void)hdc; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)x4; (void)y4;
    return 1;
}

int Chord(HDC hdc, int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4) {
    (void)hdc; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)x4; (void)y4;
    return 1;
}

int RoundRect(HDC hdc, int left, int top, int right, int bottom, int xWidth, int yHeight) {
    (void)xWidth; (void)yHeight;
    return Rectangle(hdc, left, top, right, bottom);
}

// ─── Pen/Brush functions ────────────────────────────────────────

HPEN CreatePen(int iStyle, int cWidth, uint32_t color) {
    (void)iStyle;
    // Allocate a simple pen object
    int* pen = (int*)kmalloc(sizeof(int) * 3);
    if (!pen) return NULL;
    pen[0] = OBJ_PEN;
    pen[1] = cWidth;
    pen[2] = (int)color;
    return (HPEN)pen;
}

HPEN CreatePenIndirect(const void* lplgpn) { (void)lplgpn; return CreatePen(0, 1, 0); }

HBRUSH CreateSolidBrush(uint32_t color) {
    int* brush = (int*)kmalloc(sizeof(int) * 2);
    if (!brush) return NULL;
    brush[0] = OBJ_BRUSH;
    brush[1] = (int)color;
    return (HBRUSH)brush;
}

HBRUSH CreateHatchBrush(int iStyle, uint32_t color) { (void)iStyle; return CreateSolidBrush(color); }
HBRUSH CreatePatternBrush(HBITMAP hbm) { (void)hbm; return CreateSolidBrush(0xC0C0C0); }

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc || !h) return NULL;
    
    int* obj = (int*)h;
    int type = obj[0];
    HGDIOBJ old = NULL;
    
    if (type == OBJ_PEN) {
        old = dc->cur_pen;
        dc->cur_pen = h;
        dc->pen_width = obj[1];
        dc->pen_color = (uint32_t)obj[2];
    } else if (type == OBJ_BRUSH) {
        old = dc->cur_brush;
        dc->cur_brush = h;
        dc->brush_color = (uint32_t)obj[1];
    } else if (type == OBJ_FONT) {
        old = dc->cur_font;
        dc->cur_font = h;
    }
    return old;
}

int DeleteObject(HGDIOBJ ho) {
    if (ho) kfree(ho);
    return 1;
}

int GetObject(HGDIOBJ h, int c, void* pv) {
    (void)h; (void)c; (void)pv;
    return 0;
}

HGDIOBJ GetStockObject(int i) {
    static int null_pen = OBJ_PEN;
    static int null_brush = OBJ_BRUSH;
    static int white_brush = OBJ_BRUSH;
    static int black_brush = OBJ_BRUSH;
    
    switch (i) {
        case NULL_PEN: return (HGDIOBJ)&null_pen;
        case NULL_BRUSH: return (HGDIOBJ)&null_brush;
        case WHITE_BRUSH: return (HGDIOBJ)&white_brush;
        case BLACK_BRUSH: return (HGDIOBJ)&black_brush;
        case DEFAULT_GUI_FONT: return NULL;
        default: return NULL;
    }
}

// ─── Text functions ─────────────────────────────────────────────

// Simple bitmap font for GDI text output
extern const uint8_t font8x16[96][16];

static void draw_char_simple(int x, int y, char c, uint32_t fg, uint32_t bg, int bg_mode) {
    if (c < 32 || c > 127) c = '?';
    int idx = c - 32;
    const uint8_t* glyph = font8x16[idx];
    
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                vesa_put_pixel(x + col, y + row, fg);
            } else if (bg_mode == OPAQUE) {
                vesa_put_pixel(x + col, y + row, bg);
            }
        }
    }
}

int TextOutA(HDC hdc, int x, int y, const char* lpString, int c) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc || !lpString) return 0;
    if (c < 0) { c = 0; while (lpString[c]) c++; }
    
    for (int i = 0; i < c; i++) {
        draw_char_simple(x + i * 8, y, lpString[i], dc->fg_color, dc->bg_color, dc->bg_mode);
    }
    return 1;
}

int TextOutW(HDC hdc, int x, int y, const uint16_t* lpString, int c) {
    // Convert wide chars to narrow (ASCII only)
    if (!lpString) return 0;
    if (c < 0) { c = 0; while (lpString[c]) c++; }
    char narrow[256];
    if (c > 255) c = 255;
    for (int i = 0; i < c; i++) narrow[i] = (char)(lpString[i] & 0x7F);
    narrow[c] = 0;
    return TextOutA(hdc, x, y, narrow, c);
}

int DrawTextA(HDC hdc, const char* lpchText, int cchText, RECT* lprc, uint32_t format) {
    if (!hdc || !lpchText) return 0;
    if (cchText < 0) { cchText = 0; while (lpchText[cchText]) cchText++; }
    
    int x = lprc ? lprc->left : 0;
    int y = lprc ? lprc->top : 0;
    
    // Simple: just draw the text at the rect position
    if (format & DT_CENTER) {
        int width = cchText * 8;
        if (lprc) x = lprc->left + ((lprc->right - lprc->left) - width) / 2;
    }
    
    return TextOutA(hdc, x, y, lpchText, cchText);
}

int DrawTextW(HDC hdc, const uint16_t* lpchText, int cchText, RECT* lprc, uint32_t format) {
    if (!lpchText) return 0;
    if (cchText < 0) { cchText = 0; while (lpchText[cchText]) cchText++; }
    char narrow[256];
    if (cchText > 255) cchText = 255;
    for (int i = 0; i < cchText; i++) narrow[i] = (char)(lpchText[i] & 0x7F);
    narrow[cchText] = 0;
    return DrawTextA(hdc, narrow, cchText, lprc, format);
}

// TEXTMETRIC stub
typedef struct {
    int tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    int tmAveCharWidth, tmMaxCharWidth, tmWeight;
    int tmItalic, tmUnderlined, tmStruckOut;
    int tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    int tmPitchAndFamily, tmCharSet;
} TEXTMETRICA;

int GetTextMetricsA(HDC hdc, void* lptm) {
    (void)hdc;
    if (!lptm) return 0;
    TEXTMETRICA* tm = (TEXTMETRICA*)lptm;
    tm->tmHeight = 16;
    tm->tmAscent = 12;
    tm->tmDescent = 4;
    tm->tmInternalLeading = 2;
    tm->tmExternalLeading = 2;
    tm->tmAveCharWidth = 8;
    tm->tmMaxCharWidth = 8;
    tm->tmWeight = 400;
    return 1;
}

int GetTextExtentPoint32A(HDC hdc, const char* lpString, int c, POINT* lpsz) {
    (void)hdc;
    if (!lpsz) return 0;
    if (c < 0) { c = 0; if (lpString) while (lpString[c]) c++; }
    lpsz->x = c * 8;
    lpsz->y = 16;
    return 1;
}

int SetTextAlign(HDC hdc, uint32_t fMode) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    dc->text_align = fMode;
    return 1;
}

uint32_t GetTextAlign(HDC hdc) {
    gdi_dc_t* dc = get_dc(hdc);
    return dc ? dc->text_align : 0;
}

int SetTextColor(HDC hdc, uint32_t color) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    dc->fg_color = color;
    return 1;
}

uint32_t GetTextColor(HDC hdc) {
    gdi_dc_t* dc = get_dc(hdc);
    return dc ? dc->fg_color : 0;
}

int SetBkColor(HDC hdc, uint32_t color) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    dc->bg_color = color;
    return 1;
}

uint32_t GetBkColor(HDC hdc) {
    gdi_dc_t* dc = get_dc(hdc);
    return dc ? dc->bg_color : 0;
}

int SetBkMode(HDC hdc, int iBkMode) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    dc->bg_mode = iBkMode;
    return 1;
}

// ─── Bitmap functions ───────────────────────────────────────────

HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy) {
    (void)hdc;
    // Allocate a simple bitmap header
    int* bmp = (int*)kmalloc(sizeof(int) * 4);
    if (!bmp) return NULL;
    bmp[0] = OBJ_BITMAP;
    bmp[1] = cx;
    bmp[2] = cy;
    bmp[3] = 32;  // bpp
    return (HBITMAP)bmp;
}

HBITMAP CreateBitmap(int nWidth, int nHeight, uint32_t nPlanes, uint32_t nBitsPerPel, const void* lpBits) {
    (void)nPlanes; (void)nBitsPerPel; (void)lpBits;
    return CreateCompatibleBitmap(NULL, nWidth, nHeight);
}

HBITMAP CreateDIBSection(HDC hdc, const void* pbmi, uint32_t usage, void** ppvBits, void* hSection, uint32_t offset) {
    (void)hdc; (void)pbmi; (void)usage; (void)hSection; (void)offset;
    // Allocate pixel buffer
    int w = 256, h = 256;  // default size
    if (pbmi) {
        const int* hdr = (const int*)pbmi;
        w = hdr[0]; h = hdr[1];
    }
    void* buf = kmalloc(w * h * 4);
    if (ppvBits) *ppvBits = buf;
    return CreateCompatibleBitmap(NULL, w, h);
}

int BitBlt(HDC hdcDest, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, uint32_t rop) {
    (void)hdcDest; (void)hdcSrc; (void)rop;
    // Simplified: copy from back buffer to screen
    // In reality this would use the back buffer
    return 1;
}

int StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
               HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, uint32_t rop) {
    (void)hdcDest; (void)hdcSrc; (void)rop;
    (void)xDest; (void)yDest; (void)wDest; (void)hDest;
    (void)xSrc; (void)ySrc; (void)wSrc; (void)hSrc;
    return 1;
}

// ─── Clipping functions ─────────────────────────────────────────

int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom) {
    (void)hdc; (void)left; (void)top; (void)right; (void)bottom;
    return 1;
}

int SelectClipRgn(HDC hdc, HRGN hrgn) {
    (void)hdc; (void)hrgn;
    return 1;
}

HRGN CreateRectRgn(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
    return NULL;
}

// ─── Mapping functions ──────────────────────────────────────────

int SetViewportOrgEx(HDC hdc, int x, int y, POINT* lpPoint) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    if (lpPoint) { lpPoint->x = dc->vx; lpPoint->y = dc->vy; }
    dc->vx = x; dc->vy = y;
    return 1;
}

int SetWindowOrgEx(HDC hdc, int x, int y, POINT* lpPoint) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    if (lpPoint) { lpPoint->x = dc->wx; lpPoint->y = dc->wy; }
    dc->wx = x; dc->wy = y;
    return 1;
}

int GetViewportOrgEx(HDC hdc, POINT* lpPoint) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc || !lpPoint) return 0;
    lpPoint->x = dc->vx; lpPoint->y = dc->vy;
    return 1;
}

int GetWindowOrgEx(HDC hdc, POINT* lpPoint) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc || !lpPoint) return 0;
    lpPoint->x = dc->wx; lpPoint->y = dc->wy;
    return 1;
}

// ─── DC state ───────────────────────────────────────────────────

typedef struct {
    uint32_t fg, bg, pen_color, brush_color;
    int bg_mode, text_align, pen_width;
} dc_state_t;

static dc_state_t saved_states[MAX_DC][8];

int SaveDC(HDC hdc) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc || dc->saved >= 8) return 0;
    dc_state_t* s = &saved_states[(int)(intptr_t)hdc - 1][dc->saved];
    s->fg = dc->fg_color; s->bg = dc->bg_color;
    s->pen_color = dc->pen_color; s->brush_color = dc->brush_color;
    s->bg_mode = dc->bg_mode; s->text_align = dc->text_align;
    s->pen_width = dc->pen_width;
    return ++dc->saved;
}

int RestoreDC(HDC hdc, int nSavedDC) {
    gdi_dc_t* dc = get_dc(hdc);
    if (!dc) return 0;
    if (nSavedDC < 0) dc->saved = 0;
    else dc->saved = nSavedDC - 1;
    if (dc->saved < 0) dc->saved = 0;
    dc_state_t* s = &saved_states[(int)(intptr_t)hdc - 1][dc->saved];
    dc->fg_color = s->fg; dc->bg_color = s->bg;
    dc->pen_color = s->pen_color; dc->brush_color = s->brush_color;
    dc->bg_mode = s->bg_mode; dc->text_align = s->text_align;
    dc->pen_width = s->pen_width;
    return 1;
}

// ─── Color functions ────────────────────────────────────────────

uint32_t RGB(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint8_t GetRValue(uint32_t rgb) { return (uint8_t)((rgb >> 16) & 0xFF); }
uint8_t GetGValue(uint32_t rgb) { return (uint8_t)((rgb >> 8) & 0xFF); }
uint8_t GetBValue(uint32_t rgb) { return (uint8_t)(rgb & 0xFF); }
