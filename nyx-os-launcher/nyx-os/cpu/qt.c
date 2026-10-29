// cpu/qt.c — Minimal Qt Framework implementation for Nyx OS (Pure C)
//
// Implements the core Qt classes on top of Nyx OS's VESA framebuffer,
// providing enough functionality for Telegram and other Qt applications
// to create windows, draw widgets, handle events, and make network requests.

#include <qt.h>
#include <vesa.h>
#include <desktop.h>
#include <io.h>
#include <heap.h>
#include <timer.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// ═══════════════════════════════════════════════════════════════════
// Internal helpers
// ═══════════════════════════════════════════════════════════════════

static char* qstr_dup(const char* s) {
    if (!s) return 0;
    int len = 0;
    while (s[len]) len++;
    char* r = (char*)kmalloc(len + 1);
    if (r) { memcpy(r, s, len + 1); }
    return r;
}

static int qstr_len(const char* s) {
    int n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static int qstr_eq(const char* a, const char* b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ═══════════════════════════════════════════════════════════════════
// QString Implementation (opaque: char*)
// ═══════════════════════════════════════════════════════════════════

QString qstring_new(const char* s) {
    return (QString)qstr_dup(s);
}

QString qstring_new_n(const char* s, int len) {
    if (!s || len <= 0) return 0;
    char* r = (char*)kmalloc(len + 1);
    if (r) { memcpy(r, s, len); r[len] = '\0'; }
    return (QString)r;
}

void qstring_free(QString s) {
    if (s) kfree(s);
}

const char* qstring_to_utf8(QString s) {
    return s ? (const char*)s : "";
}

int qstring_length(QString s) {
    return s ? qstr_len((const char*)s) : 0;
}

int qstring_is_empty(QString s) {
    return !s || !((const char*)s)[0];
}

int qstring_index_of(QString s, const char* sub, int from) {
    if (!s || !sub) return -1;
    const char* str = (const char*)s;
    int slen = qstr_len(sub);
    int len = qstr_len(str);
    if (from < 0) from = 0;
    for (int i = from; i <= len - slen; i++) {
        if (strncmp(str + i, sub, slen) == 0) return i;
    }
    return -1;
}

QString qstring_mid(QString s, int start, int len) {
    if (!s) return 0;
    const char* str = (const char*)s;
    int slen = qstr_len(str);
    if (start >= slen) return 0;
    if (len < 0) len = slen - start;
    if (start + len > slen) len = slen - start;
    return qstring_new_n(str + start, len);
}

QString qstring_left(QString s, int n) {
    return qstring_mid(s, 0, n);
}

QString qstring_right(QString s, int n) {
    int len = qstring_length(s);
    if (n > len) n = len;
    return qstring_mid(s, len - n, n);
}

QString qstring_replace(QString s, const char* before, const char* after) {
    if (!s || !before) return s;
    const char* str = (const char*)s;
    int blen = qstr_len(before);
    int alen = after ? qstr_len(after) : 0;
    int slen = qstr_len(str);
    char buf[4096];
    int out = 0, i = 0;
    while (i < slen && out < 4090) {
        if (strncmp(str + i, before, blen) == 0) {
            for (int j = 0; j < alen && out < 4090; j++)
                buf[out++] = after[j];
            i += blen;
        } else {
            buf[out++] = str[i++];
        }
    }
    buf[out] = '\0';
    return qstring_new(buf);
}

QString qstring_trimmed(QString s) {
    if (!s) return 0;
    const char* str = (const char*)s;
    int len = qstr_len(str);
    int start = 0, end = len - 1;
    while (start < len && (str[start] == ' ' || str[start] == '\t' || str[start] == '\n'))
        start++;
    while (end >= start && (str[end] == ' ' || str[end] == '\t' || str[end] == '\n'))
        end--;
    int nlen = end - start + 1;
    if (nlen <= 0) return 0;
    return qstring_new_n(str + start, nlen);
}

QString qstring_to_lower(QString s) {
    if (!s) return 0;
    const char* str = (const char*)s;
    int len = qstr_len(str);
    char buf[512];
    int n = len < 511 ? len : 511;
    for (int i = 0; i < n; i++)
        buf[i] = (str[i] >= 'A' && str[i] <= 'Z') ? str[i] + 32 : str[i];
    buf[n] = '\0';
    return qstring_new(buf);
}

QString qstring_to_upper(QString s) {
    if (!s) return 0;
    const char* str = (const char*)s;
    int len = qstr_len(str);
    char buf[512];
    int n = len < 511 ? len : 511;
    for (int i = 0; i < n; i++)
        buf[i] = (str[i] >= 'a' && str[i] <= 'z') ? str[i] - 32 : str[i];
    buf[n] = '\0';
    return qstring_new(buf);
}

int qstring_to_int(QString s, int* ok) {
    if (!s) { if (ok) *ok = 0; return 0; }
    const char* str = (const char*)s;
    int r = 0, sgn = 1, i = 0;
    if (str[0] == '-') { sgn = -1; i = 1; }
    else if (str[0] == '+') { i = 1; }
    int found = 0;
    while (str[i] >= '0' && str[i] <= '9') {
        r = r * 10 + (str[i] - '0');
        found = 1;
        i++;
    }
    if (ok) *ok = found;
    return r * sgn;
}

QString qstring_number(int n, int base) {
    char buf[32];
    if (base == 10) {
        int i = 0;
        if (n < 0) { buf[i++] = '-'; n = -n; }
        char tmp[16];
        int j = 0;
        do { tmp[j++] = '0' + (n % 10); n /= 10; } while (n);
        while (j > 0) buf[i++] = tmp[--j];
        buf[i] = '\0';
    } else if (base == 16) {
        buf[0] = '0'; buf[1] = 'x';
        const char* hex = "0123456789ABCDEF";
        int i = 2;
        unsigned u = (unsigned)n;
        char tmp[8];
        int j = 0;
        do { tmp[j++] = hex[u % 16]; u /= 16; } while (u);
        while (j > 0) buf[i++] = tmp[--j];
        buf[i] = '\0';
    } else {
        buf[0] = '0'; buf[1] = '\0';
    }
    return qstring_new(buf);
}

QString qstring_number_d(double n) {
    return qstring_number((int)n, 10);
}

int qstring_eq(QString s, const char* other) {
    return qstr_eq((const char*)s, other);
}

int qstring_eq_s(QString a, QString b) {
    return qstr_eq((const char*)a, (const char*)b);
}

// ═══════════════════════════════════════════════════════════════════
// QByteArray Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct { char* d; int d_size; int d_cap; } _QByteArray;

QByteArray qbytearray_new(const char* data, int size) {
    _QByteArray* a = (_QByteArray*)kmalloc(sizeof(_QByteArray));
    if (!a) return 0;
    a->d = 0; a->d_size = 0; a->d_cap = 0;
    if (data) {
        a->d_size = size < 0 ? qstr_len(data) : size;
        a->d_cap = a->d_size + 1;
        a->d = (char*)kmalloc(a->d_cap);
        if (a->d) { memcpy(a->d, data, a->d_size); a->d[a->d_size] = 0; }
    }
    return (QByteArray)a;
}

void qbytearray_free(QByteArray a) {
    if (!a) return;
    _QByteArray* ba = (_QByteArray*)a;
    if (ba->d) kfree(ba->d);
    kfree(ba);
}

const char* qbytearray_data(QByteArray a) {
    return a ? ((_QByteArray*)a)->d : "";
}

int qbytearray_size(QByteArray a) {
    return a ? ((_QByteArray*)a)->d_size : 0;
}

int qbytearray_is_empty(QByteArray a) {
    return !a || ((_QByteArray*)a)->d_size == 0;
}

QByteArray qbytearray_append(QByteArray a, const char* data, int len) {
    if (!a || !data || len <= 0) return a;
    _QByteArray* ba = (_QByteArray*)a;
    int new_size = ba->d_size + len;
    ba->d = (char*)krealloc(ba->d, new_size + 1);
    if (ba->d) {
        memcpy(ba->d + ba->d_size, data, len);
        ba->d_size = new_size;
        ba->d[new_size] = 0;
    }
    return a;
}

QByteArray qbytearray_append_c(QByteArray a, char c) {
    return qbytearray_append(a, &c, 1);
}

// ═══════════════════════════════════════════════════════════════════
// QVariant Implementation (value type, not opaque)
// ═══════════════════════════════════════════════════════════════════

QVariant qvariant_new_int(int val) {
    QVariant v; v.type = QV_Int; v.ival = val; v.sval = 0; return v;
}

QVariant qvariant_new_double(double val) {
    QVariant v; v.type = QV_Double; v.dval = val; v.sval = 0; return v;
}

QVariant qvariant_new_bool(int val) {
    QVariant v; v.type = QV_Bool; v.bval = val; v.sval = 0; return v;
}

QVariant qvariant_new_string(const char* val) {
    QVariant v; v.type = QV_String; v.ival = 0;
    v.sval = val ? qstr_dup(val) : 0;
    return v;
}

int qvariant_to_int(QVariant v) { return v.type == QV_Int ? v.ival : 0; }
double qvariant_to_double(QVariant v) { return v.type == QV_Double ? v.dval : 0; }
int qvariant_to_bool(QVariant v) { return v.type == QV_Bool ? v.bval : 0; }
QString qvariant_to_string(QVariant v) {
    if (v.type == QV_String && v.sval) return qstring_new(v.sval);
    if (v.type == QV_Int) return qstring_number(v.ival, 10);
    return 0;
}
int qvariant_is_valid(QVariant v) { return v.type != QV_Invalid; }

// ═══════════════════════════════════════════════════════════════════
// QList Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct { void** items; int count; int cap; } _QList;

QList qlist_new(void) {
    _QList* l = (_QList*)kmalloc(sizeof(_QList));
    if (l) { l->items = 0; l->count = 0; l->cap = 0; }
    return (QList)l;
}

void qlist_free(QList l) {
    if (!l) return;
    _QList* ql = (_QList*)l;
    if (ql->items) kfree(ql->items);
    kfree(ql);
}

void qlist_append(QList l, void* item) {
    if (!l) return;
    _QList* ql = (_QList*)l;
    if (ql->count >= ql->cap) {
        int newcap = ql->cap ? ql->cap * 2 : 8;
        ql->items = (void**)krealloc(ql->items, newcap * sizeof(void*));
        ql->cap = newcap;
    }
    if (ql->items) ql->items[ql->count++] = item;
}

int qlist_size(QList l) {
    return l ? ((_QList*)l)->count : 0;
}

void* qlist_at(QList l, int i) {
    if (!l) return 0;
    _QList* ql = (_QList*)l;
    if (i < 0 || i >= ql->count) return 0;
    return ql->items[i];
}

void qlist_remove_at(QList l, int i) {
    if (!l) return;
    _QList* ql = (_QList*)l;
    if (i < 0 || i >= ql->count) return;
    for (int j = i; j < ql->count - 1; j++)
        ql->items[j] = ql->items[j + 1];
    ql->count--;
}

// ═══════════════════════════════════════════════════════════════════
// QMap Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct { char* key; void* value; } _QMapEntry;
typedef struct { _QMapEntry* entries; int count; int cap; } _QMap;

QMap qmap_new(void) {
    _QMap* m = (_QMap*)kmalloc(sizeof(_QMap));
    if (m) { m->entries = 0; m->count = 0; m->cap = 0; }
    return (QMap)m;
}

void qmap_free(QMap m) {
    if (!m) return;
    _QMap* qm = (_QMap*)m;
    for (int i = 0; i < qm->count; i++) {
        if (qm->entries[i].key) kfree(qm->entries[i].key);
    }
    if (qm->entries) kfree(qm->entries);
    kfree(qm);
}

void qmap_insert(QMap m, const char* key, void* value) {
    if (!m || !key) return;
    _QMap* qm = (_QMap*)m;
    // Replace if exists
    for (int i = 0; i < qm->count; i++) {
        if (qstr_eq(qm->entries[i].key, key)) {
            qm->entries[i].value = value;
            return;
        }
    }
    // Add new
    if (qm->count >= qm->cap) {
        int newcap = qm->cap ? qm->cap * 2 : 8;
        qm->entries = (_QMapEntry*)krealloc(qm->entries, newcap * sizeof(_QMapEntry));
        qm->cap = newcap;
    }
    if (qm->entries) {
        qm->entries[qm->count].key = qstr_dup(key);
        qm->entries[qm->count].value = value;
        qm->count++;
    }
}

void* qmap_value(QMap m, const char* key, void* def) {
    if (!m || !key) return def;
    _QMap* qm = (_QMap*)m;
    for (int i = 0; i < qm->count; i++) {
        if (qstr_eq(qm->entries[i].key, key))
            return qm->entries[i].value;
    }
    return def;
}

int qmap_contains(QMap m, const char* key) {
    if (!m || !key) return 0;
    _QMap* qm = (_QMap*)m;
    for (int i = 0; i < qm->count; i++) {
        if (qstr_eq(qm->entries[i].key, key)) return 1;
    }
    return 0;
}

int qmap_size(QMap m) {
    return m ? ((_QMap*)m)->count : 0;
}

// ═══════════════════════════════════════════════════════════════════
// QObject Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct _QObject {
    struct _QObject* parent;
    QList children;
    char* name;
} _QObject;

QObject qobject_new(QObject parent) {
    _QObject* o = (_QObject*)kmalloc(sizeof(_QObject));
    if (o) {
        o->parent = (_QObject*)parent;
        o->children = qlist_new();
        o->name = 0;
        if (o->parent) qlist_append(o->parent->children, o);
    }
    return (QObject)o;
}

void qobject_free(QObject o) {
    if (!o) return;
    _QObject* qo = (_QObject*)o;
    // Remove from parent
    if (qo->parent) {
        _QList* ch = (_QList*)qo->parent->children;
        for (int i = 0; i < ch->count; i++) {
            if (ch->items[i] == o) { qlist_remove_at(qo->parent->children, i); break; }
        }
    }
    // Free children
    while (qlist_size(qo->children) > 0) {
        qobject_free(qlist_at(qo->children, 0));
    }
    qlist_free(qo->children);
    if (qo->name) kfree(qo->name);
    kfree(qo);
}

QObject qobject_parent(QObject o) {
    return o ? ((_QObject*)o)->parent : 0;
}

void qobject_set_parent(QObject o, QObject parent) {
    if (!o) return;
    _QObject* qo = (_QObject*)o;
    if (qo->parent) {
        _QList* ch = (_QList*)qo->parent->children;
        for (int i = 0; i < ch->count; i++) {
            if (ch->items[i] == o) { qlist_remove_at(qo->parent->children, i); break; }
        }
    }
    qo->parent = (_QObject*)parent;
    if (qo->parent) qlist_append(qo->parent->children, o);
}

void qobject_set_name(QObject o, const char* name) {
    if (!o) return;
    _QObject* qo = (_QObject*)o;
    if (qo->name) kfree(qo->name);
    qo->name = name ? qstr_dup(name) : 0;
}

// ═══════════════════════════════════════════════════════════════════
// QColor Implementation (value type)
// ═══════════════════════════════════════════════════════════════════

QColor qcolor_rgb(int r, int g, int b) {
    QColor c; c.rgba = 0xFF000000 | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); return c;
}

QColor qcolor_rgba(int r, int g, int b, int a) {
    QColor c; c.rgba = ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); return c;
}

QColor qcolor_hex(const char* name) {
    QColor c; c.rgba = 0;
    if (!name || name[0] != '#') return c;
    int hex = 0, i = 1;
    while (name[i]) {
        char ch = name[i]; int v = 0;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        hex = (hex << 4) | v; i++;
    }
    if (i == 7) c.rgba = 0xFF000000 | hex;
    else if (i == 9) c.rgba = hex;
    return c;
}

uint32_t qcolor_value(QColor c) { return c.rgba; }
int qcolor_red(QColor c) { return (c.rgba >> 16) & 0xFF; }
int qcolor_green(QColor c) { return (c.rgba >> 8) & 0xFF; }
int qcolor_blue(QColor c) { return c.rgba & 0xFF; }

// ═══════════════════════════════════════════════════════════════════
// QFont Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct { char* family; int pt_size; int px_size; int bold; int italic; } _QFont;

QFont qfont_new(const char* family, int size, int weight) {
    _QFont* f = (_QFont*)kmalloc(sizeof(_QFont));
    if (f) {
        f->family = qstr_dup(family ? family : "monospace");
        f->pt_size = size > 0 ? size : 12;
        f->px_size = f->pt_size + 2;
        f->bold = weight > 60 ? 1 : 0;
        f->italic = 0;
    }
    return (QFont)f;
}

void qfont_free(QFont f) {
    if (!f) return;
    _QFont* qf = (_QFont*)f;
    if (qf->family) kfree(qf->family);
    kfree(qf);
}

void qfont_set_family(QFont f, const char* family) {
    if (!f) return;
    _QFont* qf = (_QFont*)f;
    if (qf->family) kfree(qf->family);
    qf->family = qstr_dup(family ? family : "monospace");
}

void qfont_set_size(QFont f, int size) {
    if (f) { ((_QFont*)f)->pt_size = size; ((_QFont*)f)->px_size = size + 2; }
}

void qfont_set_bold(QFont f, int bold) {
    if (f) ((_QFont*)f)->bold = bold;
}

// ═══════════════════════════════════════════════════════════════════
// QPixmap Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct { uint32_t* data; int w; int h; } _QPixmap;

QPixmap qpixmap_new(int width, int height) {
    _QPixmap* p = (_QPixmap*)kmalloc(sizeof(_QPixmap));
    if (p) {
        p->w = width; p->h = height; p->data = 0;
        if (width > 0 && height > 0) {
            p->data = (uint32_t*)kmalloc(width * height * 4);
            if (p->data) memset(p->data, 0, width * height * 4);
        }
    }
    return (QPixmap)p;
}

void qpixmap_free(QPixmap p) {
    if (!p) return;
    _QPixmap* qp = (_QPixmap*)p;
    if (qp->data) kfree(qp->data);
    kfree(qp);
}

int qpixmap_width(QPixmap p) { return p ? ((_QPixmap*)p)->w : 0; }
int qpixmap_height(QPixmap p) { return p ? ((_QPixmap*)p)->h : 0; }
uint32_t* qpixmap_data(QPixmap p) { return p ? ((_QPixmap*)p)->data : 0; }
int qpixmap_is_null(QPixmap p) { return !p || !((_QPixmap*)p)->data; }

void qpixmap_fill(QPixmap p, uint32_t color) {
    if (!p) return;
    _QPixmap* qp = (_QPixmap*)p;
    if (qp->data) {
        int n = qp->w * qp->h;
        for (int i = 0; i < n; i++) qp->data[i] = color;
    }
}

// ═══════════════════════════════════════════════════════════════════
// QPainter Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct {
    void* target;
    uint32_t pen_color;
    uint32_t brush_color;
    int pen_width;
    int tx, ty;
} _QPainter;

QPainter qpainter_new(void) {
    _QPainter* p = (_QPainter*)kmalloc(sizeof(_QPainter));
    if (p) { p->target = 0; p->pen_color = 0xFFFFFFFF; p->brush_color = 0x00000000;
             p->pen_width = 1; p->tx = 0; p->ty = 0; }
    return (QPainter)p;
}

void qpainter_free(QPainter p) { if (p) kfree(p); }

void qpainter_begin_widget(QPainter p, void* widget) {
    if (p) ((_QPainter*)p)->target = widget;
}

void qpainter_end(QPainter p) { if (p) ((_QPainter*)p)->target = 0; }

void qpainter_draw_line(QPainter p, int x1, int y1, int x2, int y2) {
    if (!p) return;
    _QPainter* qp = (_QPainter*)p;
    x1 += qp->tx; y1 += qp->ty; x2 += qp->tx; y2 += qp->ty;
    int dx = x2 - x1, dy = y2 - y1;
    int steps = (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy) ? (dx > 0 ? dx : -dx) : (dy > 0 ? dy : -dy);
    if (steps == 0) { vesa_put_pixel(x1, y1, qp->pen_color); return; }
    for (int i = 0; i <= steps; i++) {
        int x = x1 + dx * i / steps, y = y1 + dy * i / steps;
        vesa_put_pixel(x, y, qp->pen_color);
    }
}

void qpainter_draw_rect(QPainter p, int x, int y, int w, int h) {
    if (!p) return;
    _QPainter* qp = (_QPainter*)p;
    vesa_draw_rect(x + qp->tx, y + qp->ty, w, h, qp->pen_color);
}

void qpainter_fill_rect(QPainter p, int x, int y, int w, int h, QColor color) {
    if (!p) return;
    _QPainter* qp = (_QPainter*)p;
    vesa_fill_rect(x + qp->tx, y + qp->ty, w, h, color.rgba);
}

void qpainter_draw_text(QPainter p, int x, int y, const char* text) {
    if (!p) return;
    _QPainter* qp = (_QPainter*)p;
    vesa_draw_string(x + qp->tx, y + qp->ty, text, qp->pen_color, qp->brush_color);
}

void qpainter_draw_pixmap(QPainter p, int x, int y, QPixmap pix) {
    if (!p || !pix) return;
    _QPainter* qp = (_QPainter*)p;
    _QPixmap* pixp = (_QPixmap*)pix;
    if (!pixp->data) return;
    for (int j = 0; j < pixp->h; j++)
        for (int i = 0; i < pixp->w; i++) {
            uint32_t c = pixp->data[j * pixp->w + i];
            if ((c & 0xFF000000) != 0)
                vesa_put_pixel(x + qp->tx + i, y + qp->ty + j, c);
        }
}

void qpainter_draw_pixmap_scaled(QPainter p, int x, int y, int w, int h, QPixmap pix) {
    if (!p || !pix) return;
    _QPainter* qp = (_QPainter*)p;
    _QPixmap* pixp = (_QPixmap*)pix;
    if (!pixp->data) return;
    for (int j = 0; j < h; j++) {
        int sy = j * pixp->h / h;
        for (int i = 0; i < w; i++) {
            int sx = i * pixp->w / w;
            uint32_t c = pixp->data[sy * pixp->w + sx];
            if ((c & 0xFF000000) != 0)
                vesa_put_pixel(x + qp->tx + i, y + qp->ty + j, c);
        }
    }
}

void qpainter_set_pen(QPainter p, QColor color) { if (p) ((_QPainter*)p)->pen_color = color.rgba; }
void qpainter_set_brush(QPainter p, QColor color) { if (p) ((_QPainter*)p)->brush_color = color.rgba; }
void qpainter_set_font(QPainter p, QFont font) { (void)p; (void)font; }
void qpainter_translate(QPainter p, int dx, int dy) {
    if (p) { ((_QPainter*)p)->tx += dx; ((_QPainter*)p)->ty += dy; }
}

// ═══════════════════════════════════════════════════════════════════
// QWidget Implementation (opaque struct)
// ═══════════════════════════════════════════════════════════════════

typedef struct _QWidget {
    _QObject base;  // inherits from QObject
    int visible;
    int enabled;
    int pos_x, pos_y, w, h;
    char* win_title;
    char* style_sheet;
    // Callbacks (type-erased)
    void (*paint_cb)(void* self, void* event);
    void (*mouse_press_cb)(void* self, void* event);
    void (*key_press_cb)(void* self, void* event);
    void (*resize_cb)(void* self, void* event);
    void (*click_cb)(void* self);
    void (*return_cb)(void* self);
    void (*change_cb)(void* self);
    void (*timer_cb)(void* self);
    // Widget-specific data
    char* text;
    int text_cap;
    int cursor_pos;
    int scroll_y;
    int checked;
    int read_only;
    int min_val, max_val, cur_val;
    int alignment;
    int word_wrap;
    int bold;
    QList items;     // for QComboBox
    void* child_w;   // for QScrollArea
    void* layout;    // for layouts
    int max_len;
    int sel_start, sel_end;
    int placeholder_cap;
    int shape, shadow;
    int line_w;
    int dropped;
    QFont font;
} _QWidget;

static _QWidget* _qw_alloc(void) {
    _QWidget* w = (_QWidget*)kmalloc(sizeof(_QWidget));
    if (!w) return 0;
    memset(w, 0, sizeof(_QWidget));
    w->base.parent = 0;
    w->base.children = qlist_new();
    w->base.name = 0;
    w->visible = 1;
    w->enabled = 1;
    w->w = 100;
    w->h = 30;
    return w;
}

QWidget qwidget_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
    }
    return (QWidget)w;
}

void qwidget_free(QWidget w) {
    if (!w) return;
    _QWidget* qw = (_QWidget*)w;
    if (qw->win_title) kfree(qw->win_title);
    if (qw->style_sheet) kfree(qw->style_sheet);
    if (qw->text) kfree(qw->text);
    if (qw->items) qlist_free(qw->items);
    if (qw->font) qfont_free(qw->font);
    if (qw->base.name) kfree(qw->base.name);
    qlist_free(qw->base.children);
    kfree(qw);
}

void qwidget_show(QWidget w) { if (w) ((_QWidget*)w)->visible = 1; }
void qwidget_hide(QWidget w) { if (w) ((_QWidget*)w)->visible = 0; }
void qwidget_close(QWidget w) { if (w) ((_QWidget*)w)->visible = 0; }
int qwidget_is_visible(QWidget w) { return w ? ((_QWidget*)w)->visible : 0; }
int qwidget_is_enabled(QWidget w) { return w ? ((_QWidget*)w)->enabled : 0; }
void qwidget_set_enabled(QWidget w, int e) { if (w) ((_QWidget*)w)->enabled = e; }
int qwidget_x(QWidget w) { return w ? ((_QWidget*)w)->pos_x : 0; }
int qwidget_y(QWidget w) { return w ? ((_QWidget*)w)->pos_y : 0; }
int qwidget_width(QWidget w) { return w ? ((_QWidget*)w)->w : 0; }
int qwidget_height(QWidget w) { return w ? ((_QWidget*)w)->h : 0; }
void qwidget_move(QWidget w, int x, int y) { if (w) { ((_QWidget*)w)->pos_x = x; ((_QWidget*)w)->pos_y = y; } }
void qwidget_resize(QWidget w, int width, int height) { if (w) { ((_QWidget*)w)->w = width; ((_QWidget*)w)->h = height; } }
void qwidget_set_geometry(QWidget w, int x, int y, int w2, int h2) { if (w) { ((_QWidget*)w)->pos_x = x; ((_QWidget*)w)->pos_y = y; ((_QWidget*)w)->w = w2; ((_QWidget*)w)->h = h2; } }

void qwidget_set_window_title(QWidget w, const char* title) {
    if (!w) return;
    _QWidget* qw = (_QWidget*)w;
    if (qw->win_title) kfree(qw->win_title);
    qw->win_title = title ? qstr_dup(title) : 0;
}

QWidget qwidget_child_at(QWidget w, int x, int y) {
    if (!w) return 0;
    _QWidget* qw = (_QWidget*)w;
    int n = qlist_size(qw->base.children);
    for (int i = n - 1; i >= 0; i--) {
        _QWidget* c = (_QWidget*)qlist_at(qw->base.children, i);
        if (c && c->visible && x >= c->pos_x && x < c->pos_x + c->w &&
            y >= c->pos_y && y < c->pos_y + c->h)
            return (QWidget)c;
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
// QLabel Implementation
// ═══════════════════════════════════════════════════════════════════

QLabel qlabel_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 20;
        w->alignment = 0x0001;
    }
    return (QLabel)w;
}

QLabel qlabel_new_text(const char* text, QWidget parent) {
    _QWidget* w = (_QWidget*)qlabel_new(parent);
    if (w && text) {
        w->text = qstr_dup(text);
        w->text_cap = qstr_len(text);
    }
    return (QLabel)w;
}

void qlabel_set_text(QLabel l, const char* text) {
    if (!l || !text) return;
    _QWidget* qw = (_QWidget*)l;
    if (qw->text) kfree(qw->text);
    qw->text = qstr_dup(text);
}

const char* qlabel_text(QLabel l) {
    return l ? ((_QWidget*)l)->text : "";
}

void qlabel_set_alignment(QLabel l, int align) { if (l) ((_QWidget*)l)->alignment = align; }
void qlabel_set_word_wrap(QLabel l, int on) { if (l) ((_QWidget*)l)->word_wrap = on; }

// ═══════════════════════════════════════════════════════════════════
// QPushButton Implementation
// ═══════════════════════════════════════════════════════════════════

QPushButton qpushbutton_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 30;
    }
    return (QPushButton)w;
}

QPushButton qpushbutton_new_text(const char* text, QWidget parent) {
    _QWidget* w = (_QWidget*)qpushbutton_new(parent);
    if (w && text) {
        w->text = qstr_dup(text);
        w->text_cap = qstr_len(text);
        w->w = qstr_len(text) * 8 + 20;
    }
    return (QPushButton)w;
}

void qpushbutton_set_text(QPushButton b, const char* text) {
    if (!b || !text) return;
    _QWidget* qw = (_QWidget*)b;
    if (qw->text) kfree(qw->text);
    qw->text = qstr_dup(text);
    qw->w = qstr_len(text) * 8 + 20;
}

void qpushbutton_set_click_callback(QPushButton b, QPushButtonCallback cb) {
    if (b) ((_QWidget*)b)->click_cb = cb;
}

// ═══════════════════════════════════════════════════════════════════
// QLineEdit Implementation
// ═══════════════════════════════════════════════════════════════════

QLineEdit qlineedit_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 24;
    }
    return (QLineEdit)w;
}

QLineEdit qlineedit_new_text(const char* text, QWidget parent) {
    _QWidget* w = (_QWidget*)qlineedit_new(parent);
    if (w && text) {
        w->text = qstr_dup(text);
        w->text_cap = qstr_len(text);
        w->cursor_pos = qstr_len(text);
    }
    return (QLineEdit)w;
}

void qlineedit_set_text(QLineEdit e, const char* text) {
    if (!e) return;
    _QWidget* qw = (_QWidget*)e;
    if (qw->text) kfree(qw->text);
    qw->text = text ? qstr_dup(text) : 0;
    qw->cursor_pos = text ? qstr_len(text) : 0;
}

const char* qlineedit_text(QLineEdit e) {
    return e ? ((_QWidget*)e)->text : "";
}

void qlineedit_set_placeholder(QLineEdit e, const char* text) {
    if (!e || !text) return;
    _QWidget* qw = (_QWidget*)e;
    // Store placeholder right after text pointer (simplified)
    // In a real impl, this would be a separate field
    (void)qw;
}

void qlineedit_set_readonly(QLineEdit e, int ro) { if (e) ((_QWidget*)e)->read_only = ro; }
void qlineedit_set_return_callback(QLineEdit e, QLineEditCallback cb) { if (e) ((_QWidget*)e)->return_cb = cb; }

// ═══════════════════════════════════════════════════════════════════
// QTextEdit Implementation
// ═══════════════════════════════════════════════════════════════════

QTextEdit qtextedit_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 100; w->w = 200;
    }
    return (QTextEdit)w;
}

QTextEdit qtextedit_new_text(const char* text, QWidget parent) {
    _QWidget* w = (_QWidget*)qtextedit_new(parent);
    if (w && text) {
        w->text = qstr_dup(text);
        w->text_cap = qstr_len(text);
    }
    return (QTextEdit)w;
}

void qtextedit_set_text(QTextEdit e, const char* text) {
    if (!e) return;
    _QWidget* qw = (_QWidget*)e;
    if (qw->text) kfree(qw->text);
    qw->text = text ? qstr_dup(text) : 0;
}

const char* qtextedit_text(QTextEdit e) {
    return e ? ((_QWidget*)e)->text : "";
}

void qtextedit_append(QTextEdit e, const char* text) {
    if (!e || !text) return;
    _QWidget* qw = (_QWidget*)e;
    int old_len = qw->text ? qstr_len(qw->text) : 0;
    int add_len = qstr_len(text);
    int new_len = old_len + add_len;
    qw->text = (char*)krealloc(qw->text, new_len + 1);
    if (qw->text) {
        memcpy(qw->text + old_len, text, add_len + 1);
    }
}

void qtextedit_set_readonly(QTextEdit e, int ro) { if (e) ((_QWidget*)e)->read_only = ro; }

// ═══════════════════════════════════════════════════════════════════
// QScrollArea Implementation
// ═══════════════════════════════════════════════════════════════════

QScrollArea qscrollarea_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
    }
    return (QScrollArea)w;
}

void qscrollarea_set_widget(QScrollArea a, QWidget w) {
    if (a) ((_QWidget*)a)->child_w = w;
}

int qscrollarea_scroll_y(QScrollArea a) {
    return a ? ((_QWidget*)a)->scroll_y : 0;
}

void qscrollarea_set_scroll_y(QScrollArea a, int val) {
    if (a) ((_QWidget*)a)->scroll_y = val;
}

// ═══════════════════════════════════════════════════════════════════
// QFrame, QProgressBar, QCheckBox, QComboBox, QSlider
// ═══════════════════════════════════════════════════════════════════

QFrame qframe_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->shape = 1; // StyledPanel
    }
    return (QFrame)w;
}

void qframe_set_line_width(QFrame f, int w) { if (f) ((_QWidget*)f)->line_w = w; }

QProgressBar qprogressbar_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 20;
        w->max_val = 100;
    }
    return (QProgressBar)w;
}

void qprogressbar_set_range(QProgressBar p, int min, int max) {
    if (!p) return;
    _QWidget* qw = (_QWidget*)p;
    qw->min_val = min; qw->max_val = max;
}

void qprogressbar_set_value(QProgressBar p, int val) {
    if (p) ((_QWidget*)p)->cur_val = val;
}

QCheckBox qcheckbox_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 20;
    }
    return (QCheckBox)w;
}

QCheckBox qcheckbox_new_text(const char* text, QWidget parent) {
    _QWidget* w = (_QWidget*)qcheckbox_new(parent);
    if (w && text) {
        w->text = qstr_dup(text);
        w->text_cap = qstr_len(text);
    }
    return (QCheckBox)w;
}

int qcheckbox_is_checked(QCheckBox c) { return c ? ((_QWidget*)c)->checked : 0; }
void qcheckbox_set_checked(QCheckBox c, int checked) { if (c) ((_QWidget*)c)->checked = checked; }

QComboBox qcombobox_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 24;
        w->items = qlist_new();
        w->cur_val = -1;
    }
    return (QComboBox)w;
}

void qcombobox_add_item(QComboBox cb, const char* text) {
    if (!cb || !text) return;
    _QWidget* qw = (_QWidget*)cb;
    if (!qw->items) qw->items = qlist_new();
    qlist_append(qw->items, qstr_dup(text));
}

int qcombobox_current_index(QComboBox cb) { return cb ? ((_QWidget*)cb)->cur_val : -1; }

void qcombobox_set_current_index(QComboBox cb, int idx) { if (cb) ((_QWidget*)cb)->cur_val = idx; }

const char* qcombobox_current_text(QComboBox cb) {
    if (!cb) return "";
    _QWidget* qw = (_QWidget*)cb;
    if (qw->cur_val >= 0 && qw->cur_val < qlist_size(qw->items))
        return (const char*)qlist_at(qw->items, qw->cur_val);
    return "";
}

QSlider qslider_new(QWidget parent) {
    _QWidget* w = _qw_alloc();
    if (w) {
        w->base.parent = (_QObject*)parent;
        if (w->base.parent) qlist_append(w->base.parent->children, w);
        w->h = 20; w->w = 200;
        w->max_val = 100;
    }
    return (QSlider)w;
}

void qslider_set_range(QSlider s, int min, int max) {
    if (s) { ((_QWidget*)s)->min_val = min; ((_QWidget*)s)->max_val = max; }
}

void qslider_set_value(QSlider s, int val) { if (s) ((_QWidget*)s)->cur_val = val; }
int qslider_value(QSlider s) { return s ? ((_QWidget*)s)->cur_val : 0; }

// ═══════════════════════════════════════════════════════════════════
// Layout Implementations
// ═══════════════════════════════════════════════════════════════════

typedef struct { QList widgets; int is_vbox; QWidget parent_w; } _QLayout;

QLayout qlayout_new_vbox(QWidget parent) {
    _QLayout* l = (_QLayout*)kmalloc(sizeof(_QLayout));
    if (l) { l->widgets = qlist_new(); l->is_vbox = 1; l->parent_w = parent; }
    return (QLayout)l;
}

QLayout qlayout_new_hbox(QWidget parent) {
    _QLayout* l = (_QLayout*)kmalloc(sizeof(_QLayout));
    if (l) { l->widgets = qlist_new(); l->is_vbox = 0; l->parent_w = parent; }
    return (QLayout)l;
}

void qlayout_add_widget(QLayout l, QWidget w) {
    if (l && w) qlist_append(((_QLayout*)l)->widgets, w);
}

void qlayout_activate(QLayout l) {
    if (!l) return;
    _QLayout* ql = (_QLayout*)l;
    int pos = 0;
    int n = qlist_size(ql->widgets);
    for (int i = 0; i < n; i++) {
        _QWidget* w = (_QWidget*)qlist_at(ql->widgets, i);
        if (!w) continue;
        if (ql->is_vbox) { w->pos_x = 0; w->pos_y = pos; pos += w->h + 4; }
        else { w->pos_x = pos; w->pos_y = 0; pos += w->w + 4; }
    }
}

// ═══════════════════════════════════════════════════════════════════
// QApplication Implementation
// ═══════════════════════════════════════════════════════════════════

typedef struct { _QObject base; int running; } _QApp;
static _QApp* _app_inst = 0;

QApplication qapp_new(int* argc, char** argv) {
    (void)argc; (void)argv;
    _QApp* a = (_QApp*)kmalloc(sizeof(_QApp));
    if (a) { a->base.parent = 0; a->base.children = qlist_new(); a->base.name = 0; a->running = 1; }
    _app_inst = a;
    return (QApplication)a;
}

void qapp_free(QApplication a) {
    if (a) { kfree(a); _app_inst = 0; }
}

int qapp_exec(QApplication a) {
    if (!a) return 0;
    _QApp* qa = (_QApp*)a;
    qa->running = 1;
    while (qa->running) {
        // Process events (placeholder)
        asm volatile("hlt");
    }
    return 0;
}

void qapp_quit(QApplication a) {
    if (a) ((_QApp*)a)->running = 0;
}

int qapp_desktop_width(QApplication a) { (void)a; return vesa.width; }
int qapp_desktop_height(QApplication a) { (void)a; return vesa.height; }

// ═══════════════════════════════════════════════════════════════════
// QTimer Implementation
// ═══════════════════════════════════════════════════════════════════

typedef struct { _QObject base; int active; int interval_ms; QTimerCallback cb; } _QTimer;

QTimer qtimer_new(QObject parent) {
    _QTimer* t = (_QTimer*)kmalloc(sizeof(_QTimer));
    if (t) { t->base.parent = (_QObject*)parent; t->base.children = qlist_new();
             t->base.name = 0; t->active = 0; t->interval_ms = 0; t->cb = 0; }
    return (QTimer)t;
}

void qtimer_free(QTimer t) { if (t) kfree(t); }
void qtimer_start(QTimer t, int msec) { if (t) { ((_QTimer*)t)->active = 1; ((_QTimer*)t)->interval_ms = msec; } }
void qtimer_stop(QTimer t) { if (t) ((_QTimer*)t)->active = 0; }
void qtimer_set_callback(QTimer t, QTimerCallback cb) { if (t) ((_QTimer*)t)->cb = cb; }

// ═══════════════════════════════════════════════════════════════════
// QCoreApplication
// ═══════════════════════════════════════════════════════════════════

int qcoreapplication_exec(void) {
    if (_app_inst) return qapp_exec((QApplication)_app_inst);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
// Network (stubs)
// ═══════════════════════════════════════════════════════════════════

QNetworkRequest qnetworkrequest_new(const char* url) { (void)url; return 0; }
void qnetworkrequest_free(QNetworkRequest r) { (void)r; }
void qnetworkrequest_set_url(QNetworkRequest r, const char* url) { (void)r; (void)url; }
QNetworkReply qnetworkreply_new(void) { return 0; }
void qnetworkreply_free(QNetworkReply r) { (void)r; }
QByteArray qnetworkreply_read_all(QNetworkReply r) { (void)r; return 0; }
int qnetworkreply_error(QNetworkReply r) { (void)r; return -1; }
int qnetworkreply_is_finished(QNetworkReply r) { (void)r; return 1; }
QNetworkAccessManager qnetworkaccessmanager_new(QObject parent) { (void)parent; return 0; }
void qnetworkaccessmanager_set_callback(QNetworkAccessManager m, QNetworkReplyCallback cb) { (void)m; (void)cb; }
QNetworkReply qnetworkaccessmanager_get(QNetworkAccessManager m, QNetworkRequest r) { (void)m; (void)r; return 0; }
QNetworkReply qnetworkaccessmanager_post(QNetworkAccessManager m, QNetworkRequest r, const char* data, int len) {
    (void)m; (void)r; (void)data; (void)len; return 0;
}

// ═══════════════════════════════════════════════════════════════════
// Logging
// ═══════════════════════════════════════════════════════════════════

static void q_simple_vsprintf(char* out, int maxlen, const char* fmt, va_list args) {
    int pos = 0;
    for (int i = 0; fmt[i] && pos < maxlen - 1; i++) {
        if (fmt[i] != '%') { out[pos++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == 's') {
            const char* s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s && pos < maxlen - 1) out[pos++] = *s++;
        } else if (fmt[i] == 'd') {
            int val = va_arg(args, int);
            if (val < 0) { out[pos++] = '-'; val = -val; }
            char tmp[12]; int j = 0;
            do { tmp[j++] = '0' + (val % 10); val /= 10; } while (val);
            while (j > 0 && pos < maxlen - 1) out[pos++] = tmp[--j];
        } else if (fmt[i] == 'x') {
            unsigned val = (unsigned)va_arg(args, unsigned);
            const char* hex = "0123456789abcdef";
            char tmp[8]; int j = 0;
            do { tmp[j++] = hex[val & 0xF]; val >>= 4; } while (val);
            while (j > 0 && pos < maxlen - 1) out[pos++] = tmp[--j];
        } else if (fmt[i] == 'c') {
            out[pos++] = (char)va_arg(args, int);
        } else if (fmt[i] == '%') {
            out[pos++] = '%';
        } else {
            out[pos++] = '%'; out[pos++] = fmt[i];
        }
    }
    out[pos] = '\0';
}

static void q_log(const char* prefix, const char* fmt, va_list args) {
    char buf[1024];
    int pos = 0;
    while (prefix[pos] && pos < 31) { buf[pos] = prefix[pos]; pos++; }
    q_simple_vsprintf(buf + pos, sizeof(buf) - pos, fmt, args);
    for (int i = 0; buf[i]; i++) {
        int sp = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && sp++ < 200000) {}
        outb(0x3F8, (uint8_t)buf[i]);
    }
}

void qdebug(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    q_log("[QT] ", fmt, args);
    va_end(args);
}

void qwarning(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    q_log("[QT WARN] ", fmt, args);
    va_end(args);
}

void qcritical(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    q_log("[QT CRIT] ", fmt, args);
    va_end(args);
}
