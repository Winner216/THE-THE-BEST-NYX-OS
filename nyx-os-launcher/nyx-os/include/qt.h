#ifndef QT_H
#define QT_H

// ─── Minimal Qt Framework for Nyx OS (Pure C API) ───────────────
// Provides the core Qt classes needed by Telegram and other Qt apps.
// Uses opaque handles and C function-pointer APIs for compatibility
// with the C kernel.

#include <stdint.h>
#include <stddef.h>

// ═══════════════════════════════════════════════════════════════════
// Core Types (C-compatible)
// ═══════════════════════════════════════════════════════════════════

// ─── QString (opaque handle) ────────────────────────────────────
typedef void* QString;

QString  qstring_new(const char* s);
QString  qstring_new_n(const char* s, int len);
void     qstring_free(QString s);
const char* qstring_to_utf8(QString s);
int      qstring_length(QString s);
int      qstring_is_empty(QString s);
int      qstring_index_of(QString s, const char* sub, int from);
QString  qstring_mid(QString s, int start, int len);
QString  qstring_left(QString s, int n);
QString  qstring_right(QString s, int n);
QString  qstring_replace(QString s, const char* before, const char* after);
QString  qstring_trimmed(QString s);
QString  qstring_to_lower(QString s);
QString  qstring_to_upper(QString s);
int      qstring_to_int(QString s, int* ok);
QString  qstring_number(int n, int base);
QString  qstring_number_d(double n);
int      qstring_eq(QString s, const char* other);
int      qstring_eq_s(QString a, QString b);

// ─── QByteArray ─────────────────────────────────────────────────
typedef void* QByteArray;

QByteArray qbytearray_new(const char* data, int size);
void       qbytearray_free(QByteArray a);
const char* qbytearray_data(QByteArray a);
int        qbytearray_size(QByteArray a);
int        qbytearray_is_empty(QByteArray a);
QByteArray qbytearray_append(QByteArray a, const char* data, int len);
QByteArray qbytearray_append_c(QByteArray a, char c);

// ─── QVariant ───────────────────────────────────────────────────
typedef struct {
    enum { QV_Invalid, QV_Bool, QV_Int, QV_Double, QV_String } type;
    union { int ival; double dval; int bval; };
    char* sval;
} QVariant;

QVariant qvariant_new_int(int val);
QVariant qvariant_new_double(double val);
QVariant qvariant_new_bool(int val);
QVariant qvariant_new_string(const char* val);
int      qvariant_to_int(QVariant v);
double   qvariant_to_double(QVariant v);
int      qvariant_to_bool(QVariant v);
QString  qvariant_to_string(QVariant v);
int      qvariant_is_valid(QVariant v);

// ─── QList (type-erased) ────────────────────────────────────────
typedef void* QList;

QList   qlist_new(void);
void    qlist_free(QList l);
void    qlist_append(QList l, void* item);
int     qlist_size(QList l);
void*   qlist_at(QList l, int i);
void    qlist_remove_at(QList l, int i);

// ─── QMap ───────────────────────────────────────────────────────
typedef void* QMap;

QMap    qmap_new(void);
void    qmap_free(QMap m);
void    qmap_insert(QMap m, const char* key, void* value);
void*   qmap_value(QMap m, const char* key, void* def);
int     qmap_contains(QMap m, const char* key);
int     qmap_size(QMap m);

// ═══════════════════════════════════════════════════════════════════
// Core Classes (opaque handles)
// ═══════════════════════════════════════════════════════════════════

// ─── QObject ────────────────────────────────────────────────────
typedef void* QObject;

QObject  qobject_new(QObject parent);
void     qobject_free(QObject o);
QObject  qobject_parent(QObject o);
void     qobject_set_parent(QObject o, QObject parent);
void     qobject_set_name(QObject o, const char* name);

// ─── QEvent ─────────────────────────────────────────────────────
typedef struct {
    int type;
} QEvent;

typedef struct {
    int type; int x; int y; int button;
} QMouseEvent;

typedef struct {
    int type; int key; int modifiers; char text[64];
} QKeyEvent;

typedef struct {
    int type;
} QPaintEvent;

typedef struct {
    int type; int width; int height;
} QResizeEvent;

typedef struct {
    int type; int timer_id;
} QTimerEvent;

// Event types
#define QE_None 0
#define QE_Timer 1
#define QE_MouseButtonPress 2
#define QE_MouseButtonRelease 3
#define QE_MouseMove 5
#define QE_KeyPress 6
#define QE_KeyRelease 7
#define QE_Resize 8
#define QE_Paint 9
#define QE_Close 10

// Key codes
#define QKey_Escape 0x01000000
#define QKey_Tab 0x01000001
#define QKey_Return 0x01000004
#define QKey_Backspace 0x01000003
#define QKey_Delete 0x01000007
#define QKey_Left 0x01000012
#define QKey_Right 0x01000014
#define QKey_Up 0x01000013
#define QKey_Down 0x01000015
#define QKey_Shift 0x02000000
#define QKey_Control 0x04000000
#define QKey_Alt 0x08000000

// ═══════════════════════════════════════════════════════════════════
// GUI Types
// ═══════════════════════════════════════════════════════════════════

// ─── QColor ─────────────────────────────────────────────────────
typedef struct { uint32_t rgba; } QColor;

QColor qcolor_rgb(int r, int g, int b);
QColor qcolor_rgba(int r, int g, int b, int a);
QColor qcolor_hex(const char* name);
uint32_t qcolor_value(QColor c);
int    qcolor_red(QColor c);
int    qcolor_green(QColor c);
int    qcolor_blue(QColor c);

// Named colors
#define QC_Black     qcolor_rgb(0,0,0)
#define QC_White     qcolor_rgb(255,255,255)
#define QC_Red       qcolor_rgb(255,0,0)
#define QC_Green     qcolor_rgb(0,128,0)
#define QC_Blue      qcolor_rgb(0,0,255)
#define QC_Gray      qcolor_rgb(128,128,128)
#define QC_LightGray qcolor_rgb(192,192,192)
#define QC_DarkGray  qcolor_rgb(64,64,64)

// ─── QFont ──────────────────────────────────────────────────────
typedef void* QFont;

QFont   qfont_new(const char* family, int size, int weight);
void    qfont_free(QFont f);
void    qfont_set_family(QFont f, const char* family);
void    qfont_set_size(QFont f, int size);
void    qfont_set_bold(QFont f, int bold);

// ─── QPixmap ────────────────────────────────────────────────────
typedef void* QPixmap;

QPixmap qpixmap_new(int width, int height);
void    qpixmap_free(QPixmap p);
int     qpixmap_width(QPixmap p);
int     qpixmap_height(QPixmap p);
uint32_t* qpixmap_data(QPixmap p);
int     qpixmap_is_null(QPixmap p);
void    qpixmap_fill(QPixmap p, uint32_t color);

// ═══════════════════════════════════════════════════════════════════
// Painting
// ═══════════════════════════════════════════════════════════════════

typedef void* QPainter;

QPainter qpainter_new(void);
void     qpainter_free(QPainter p);
void     qpainter_begin_widget(QPainter p, void* widget);
void     qpainter_end(QPainter p);
void     qpainter_draw_line(QPainter p, int x1, int y1, int x2, int y2);
void     qpainter_draw_rect(QPainter p, int x, int y, int w, int h);
void     qpainter_fill_rect(QPainter p, int x, int y, int w, int h, QColor color);
void     qpainter_draw_text(QPainter p, int x, int y, const char* text);
void     qpainter_draw_pixmap(QPainter p, int x, int y, QPixmap pix);
void     qpainter_draw_pixmap_scaled(QPainter p, int x, int y, int w, int h, QPixmap pix);
void     qpainter_set_pen(QPainter p, QColor color);
void     qpainter_set_brush(QPainter p, QColor color);
void     qpainter_set_font(QPainter p, QFont font);
void     qpainter_translate(QPainter p, int dx, int dy);

// ═══════════════════════════════════════════════════════════════════
// Widget Classes
// ═══════════════════════════════════════════════════════════════════

// ─── QWidget ────────────────────────────────────────────────────
typedef void* QWidget;

QWidget  qwidget_new(QWidget parent);
void     qwidget_free(QWidget w);
void     qwidget_show(QWidget w);
void     qwidget_hide(QWidget w);
void     qwidget_close(QWidget w);
int      qwidget_is_visible(QWidget w);
int      qwidget_is_enabled(QWidget w);
void     qwidget_set_enabled(QWidget w, int e);
int      qwidget_x(QWidget w);
int      qwidget_y(QWidget w);
int      qwidget_width(QWidget w);
int      qwidget_height(QWidget w);
void     qwidget_move(QWidget w, int x, int y);
void     qwidget_resize(QWidget w, int width, int height);
void     qwidget_set_geometry(QWidget w, int x, int y, int w2, int h2);
void     qwidget_set_window_title(QWidget w, const char* title);
QWidget  qwidget_child_at(QWidget w, int x, int y);

// ─── QLabel ─────────────────────────────────────────────────────
typedef void* QLabel;

QLabel   qlabel_new(QWidget parent);
QLabel   qlabel_new_text(const char* text, QWidget parent);
void     qlabel_set_text(QLabel l, const char* text);
const char* qlabel_text(QLabel l);
void     qlabel_set_alignment(QLabel l, int align);
void     qlabel_set_word_wrap(QLabel l, int on);

// ─── QPushButton ────────────────────────────────────────────────
typedef void* QPushButton;

QPushButton qpushbutton_new(QWidget parent);
QPushButton qpushbutton_new_text(const char* text, QWidget parent);
void        qpushbutton_set_text(QPushButton b, const char* text);
typedef void (*QPushButtonCallback)(QPushButton);
void        qpushbutton_set_click_callback(QPushButton b, QPushButtonCallback cb);

// ─── QLineEdit ──────────────────────────────────────────────────
typedef void* QLineEdit;

QLineEdit qlineedit_new(QWidget parent);
QLineEdit qlineedit_new_text(const char* text, QWidget parent);
void      qlineedit_set_text(QLineEdit e, const char* text);
const char* qlineedit_text(QLineEdit e);
void      qlineedit_set_placeholder(QLineEdit e, const char* text);
void      qlineedit_set_readonly(QLineEdit e, int ro);
typedef void (*QLineEditCallback)(QLineEdit);
void      qlineedit_set_return_callback(QLineEdit e, QLineEditCallback cb);

// ─── QTextEdit ──────────────────────────────────────────────────
typedef void* QTextEdit;

QTextEdit qtextedit_new(QWidget parent);
QTextEdit qtextedit_new_text(const char* text, QWidget parent);
void      qtextedit_set_text(QTextEdit e, const char* text);
const char* qtextedit_text(QTextEdit e);
void      qtextedit_append(QTextEdit e, const char* text);
void      qtextedit_set_readonly(QTextEdit e, int ro);

// ─── QScrollArea ────────────────────────────────────────────────
typedef void* QScrollArea;

QScrollArea qscrollarea_new(QWidget parent);
void        qscrollarea_set_widget(QScrollArea a, QWidget w);
int         qscrollarea_scroll_y(QScrollArea a);
void        qscrollarea_set_scroll_y(QScrollArea a, int val);

// ─── QFrame ─────────────────────────────────────────────────────
typedef void* QFrame;

QFrame  qframe_new(QWidget parent);
void    qframe_set_line_width(QFrame f, int w);

// ─── QProgressBar ───────────────────────────────────────────────
typedef void* QProgressBar;

QProgressBar qprogressbar_new(QWidget parent);
void         qprogressbar_set_range(QProgressBar p, int min, int max);
void         qprogressbar_set_value(QProgressBar p, int val);

// ─── QCheckBox ──────────────────────────────────────────────────
typedef void* QCheckBox;

QCheckBox qcheckbox_new(QWidget parent);
QCheckBox qcheckbox_new_text(const char* text, QWidget parent);
int       qcheckbox_is_checked(QCheckBox c);
void      qcheckbox_set_checked(QCheckBox c, int checked);

// ─── QComboBox ──────────────────────────────────────────────────
typedef void* QComboBox;

QComboBox qcombobox_new(QWidget parent);
void      qcombobox_add_item(QComboBox cb, const char* text);
int       qcombobox_current_index(QComboBox cb);
void      qcombobox_set_current_index(QComboBox cb, int idx);
const char* qcombobox_current_text(QComboBox cb);

// ─── QSlider ────────────────────────────────────────────────────
typedef void* QSlider;

QSlider qslider_new(QWidget parent);
void    qslider_set_range(QSlider s, int min, int max);
void    qslider_set_value(QSlider s, int val);
int     qslider_value(QSlider s);

// ═══════════════════════════════════════════════════════════════════
// Layout
// ═══════════════════════════════════════════════════════════════════

typedef void* QLayout;

QLayout qlayout_new_vbox(QWidget parent);
QLayout qlayout_new_hbox(QWidget parent);
void    qlayout_add_widget(QLayout l, QWidget w);
void    qlayout_activate(QLayout l);

// ═══════════════════════════════════════════════════════════════════
// Application & Event Loop
// ═══════════════════════════════════════════════════════════════════

typedef void* QApplication;

QApplication qapp_new(int* argc, char** argv);
void         qapp_free(QApplication a);
int          qapp_exec(QApplication a);
void         qapp_quit(QApplication a);
int          qapp_desktop_width(QApplication a);
int          qapp_desktop_height(QApplication a);

// ─── QTimer ─────────────────────────────────────────────────────
typedef void* QTimer;

QTimer  qtimer_new(QObject parent);
void    qtimer_free(QTimer t);
void    qtimer_start(QTimer t, int msec);
void    qtimer_stop(QTimer t);
typedef void (*QTimerCallback)(QTimer);
void    qtimer_set_callback(QTimer t, QTimerCallback cb);

// ─── QCoreApplication ───────────────────────────────────────────
int qcoreapplication_exec(void);

// ═══════════════════════════════════════════════════════════════════
// Network
// ═══════════════════════════════════════════════════════════════════

typedef void* QNetworkRequest;
typedef void* QNetworkReply;
typedef void* QNetworkAccessManager;

QNetworkRequest qnetworkrequest_new(const char* url);
void            qnetworkrequest_free(QNetworkRequest r);
void            qnetworkrequest_set_url(QNetworkRequest r, const char* url);

QNetworkReply  qnetworkreply_new(void);
void           qnetworkreply_free(QNetworkReply r);
QByteArray     qnetworkreply_read_all(QNetworkReply r);
int            qnetworkreply_error(QNetworkReply r);
int            qnetworkreply_is_finished(QNetworkReply r);

QNetworkAccessManager qnetworkaccessmanager_new(QObject parent);
typedef void (*QNetworkReplyCallback)(QNetworkReply);
void qnetworkaccessmanager_set_callback(QNetworkAccessManager m, QNetworkReplyCallback cb);
QNetworkReply qnetworkaccessmanager_get(QNetworkAccessManager m, QNetworkRequest r);
QNetworkReply qnetworkaccessmanager_post(QNetworkAccessManager m, QNetworkRequest r, const char* data, int len);

// ═══════════════════════════════════════════════════════════════════
// Logging
// ═══════════════════════════════════════════════════════════════════

void qdebug(const char* fmt, ...);
void qwarning(const char* fmt, ...);
void qcritical(const char* fmt, ...);

#endif /* QT_H */
