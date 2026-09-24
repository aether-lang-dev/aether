/* Test fixture: a native top-level window for the GPU presentation tests.
 * See native_window.h for why it exists and what it is not.
 */

#include "native_window.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  define TW_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define TW_THREAD_LOCAL __thread
#else
#  define TW_THREAD_LOCAL
#endif

static TW_THREAD_LOCAL char g_err[512];

static int tw_fail(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return code;
}

static void tw_clear_error(void) { g_err[0] = '\0'; }

const char* tw_last_error(void) { return g_err; }

/* ======================================================================== */
/* Windows                                                                   */
/* ======================================================================== */
#if defined(_WIN32)

#include <windows.h>

/* Asks PrintWindow for what DWM composes, which is where a flip-model
 * swapchain's frames live; without it the capture is the GDI surface, which a
 * GPU never draws into. Windows 8.1 and later; older SDK headers lack the
 * name. */
#ifndef PW_RENDERFULLCONTENT
#  define PW_RENDERFULLCONTENT 0x00000002
#endif

struct TwWindow {
    HWND hwnd;
    int  width, height;
    int  close_requested;
    int  resized;
};

static const wchar_t k_class_name[] = L"AetherContribWindow";
static INIT_ONCE g_class_once = INIT_ONCE_STATIC_INIT;
static ATOM      g_class;

static LRESULT CALLBACK tw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = (const CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    TwWindow* w = (TwWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CLOSE:
            /* The window is the caller's to destroy: closing only says the
             * user asked, which tw_pump reports. */
            if (w) w->close_requested = 1;
            return 0;
        case WM_SIZE:
            if (w) {
                int nw = (int)LOWORD(lp), nh = (int)HIWORD(lp);
                if (nw != w->width || nh != w->height) {
                    w->width = nw;
                    w->height = nh;
                    w->resized = 1;
                }
            }
            return 0;
        case WM_ERASEBKGND:
            /* The GPU owns every pixel of the client area; letting GDI paint
             * the class background would flash it between presents. */
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static BOOL CALLBACK tw_register_class(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = tw_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = k_class_name;
    g_class = RegisterClassExW(&wc);
    return g_class != 0;
}

int tw_available(void) { return 1; }

static void tw_win32_pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

TwWindow* tw_create(const char* title, int width, int height) {
    tw_clear_error();
    if (width <= 0 || height <= 0) {
        tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
        return NULL;
    }
    if (!InitOnceExecuteOnce(&g_class_once, tw_register_class, NULL, NULL) || !g_class) {
        tw_fail(TW_ERR_SYSTEM, "RegisterClassExW failed (error %lu)",
                   (unsigned long)GetLastError());
        return NULL;
    }

    wchar_t wtitle[256];
    wtitle[0] = 0;
    if (title && *title) {
        if (!MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle,
                                 (int)(sizeof(wtitle) / sizeof(wtitle[0])))) {
            wtitle[0] = 0;
        }
    }

    TwWindow* w = (TwWindow*)calloc(1, sizeof(*w));
    if (!w) { tw_fail(TW_ERR_OOM, "out of memory"); return NULL; }

    /* The size asked for is the CLIENT area, which is what a swapchain gets;
     * the frame and title bar are added around it. */
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, width, height };
    AdjustWindowRectEx(&r, style, FALSE, 0);
    w->hwnd = CreateWindowExW(0, k_class_name, wtitle, style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, GetModuleHandleW(NULL), w);
    if (!w->hwnd) {
        tw_fail(TW_ERR_SYSTEM, "CreateWindowExW failed (error %lu)",
                   (unsigned long)GetLastError());
        free(w);
        return NULL;
    }
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    UpdateWindow(w->hwnd);

    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->width = cr.right - cr.left;
    w->height = cr.bottom - cr.top;
    w->resized = 0;
    tw_win32_pump_messages();
    w->resized = 0;
    return w;
}

void tw_destroy(TwWindow* w) {
    if (!w) return;
    if (w->hwnd) {
        /* Detach first: DestroyWindow sends messages that would otherwise
         * reach a struct about to be freed. */
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(w->hwnd);
        tw_win32_pump_messages();
    }
    free(w);
}

int tw_pump(TwWindow* w) {
    if (!w) return 0;
    tw_win32_pump_messages();
    return w->close_requested ? 0 : 1;
}

int tw_set_size(TwWindow* w, int width, int height) {
    tw_clear_error();
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    if (width <= 0 || height <= 0) {
        return tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
    }
    RECT r = { 0, 0, width, height };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongW(w->hwnd, GWL_EXSTYLE));
    /* WM_SIZE is SENT, not posted, so the new size is recorded by the time
     * SetWindowPos returns. */
    if (!SetWindowPos(w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return tw_fail(TW_ERR_SYSTEM, "SetWindowPos failed (error %lu)",
                          (unsigned long)GetLastError());
    }
    tw_win32_pump_messages();
    return TW_OK;
}

int tw_set_minimized(TwWindow* w, int on) {
    tw_clear_error();
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    /* WM_SIZE is sent during ShowWindow, so the size (0 x 0 when minimised)
     * is recorded by the time this returns. */
    ShowWindow(w->hwnd, on ? SW_MINIMIZE : SW_RESTORE);
    tw_win32_pump_messages();
    return TW_OK;
}

int   tw_kind(const TwWindow* w)    { return w ? TW_KIND_WIN32 : TW_KIND_NONE; }
void* tw_handle(const TwWindow* w)  { return w ? (void*)w->hwnd : NULL; }
void* tw_display(const TwWindow* w) { (void)w; return NULL; }

int tw_pixel(TwWindow* w, int x, int y) {
    tw_clear_error();
    if (!w) { tw_fail(TW_ERR_ARG, "window is null"); return -1; }
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) {
        tw_fail(TW_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, w->width, w->height);
        return -1;
    }
    HDC wdc = GetDC(w->hwnd);
    if (!wdc) { tw_fail(TW_ERR_SYSTEM, "GetDC failed"); return -1; }
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w->width;
    bi.bmiHeader.biHeight = -w->height;   /* top-down rows */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = mem ? CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    int result = -1;
    if (!mem || !bmp || !bits) {
        tw_fail(TW_ERR_SYSTEM, "cannot create a capture bitmap");
    } else {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (!PrintWindow(w->hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) {
            tw_fail(TW_ERR_SYSTEM, "PrintWindow failed (error %lu)",
                       (unsigned long)GetLastError());
        } else {
            GdiFlush();
            const unsigned char* px =
                (const unsigned char*)bits + ((size_t)y * (size_t)w->width + (size_t)x) * 4u;
            /* A 32-bit DIB is B, G, R, unused. */
            result = (int)(((unsigned)px[2] << 16) | ((unsigned)px[1] << 8) |
                           (unsigned)px[0]);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(w->hwnd, wdc);
    return result;
}

/* ======================================================================== */
/* macOS: AppKit, through the Objective-C runtime                            */
/* ======================================================================== */
#elif defined(__APPLE__)

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <objc/runtime.h>

/* NSRect and NSSize as the 64-bit ABI lays them out (CGFloat is double on
 * every macOS this runs on). They cross objc_msgSend by value, so the layout
 * is the contract, not the name. */
typedef struct { double x, y, w, h; } TwRect;
typedef struct { double w, h; } TwSize;

static struct {
    int    loaded;          /* 0 unprobed, 1 usable, -1 not */
    void*  objc;
    void*  appkit;
    Class  (*get_class)(const char*);
    SEL    (*sel)(const char*);
    void*  send;
    void*  send_stret;      /* struct returns above 16 bytes on x86_64 */
    void*  (*pool_push)(void);
    void   (*pool_pop)(void*);
    id*    default_mode;    /* NSDefaultRunLoopMode */
    id     app;
} g_mac;

#define TW_SEND(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))g_mac.send)
#define TW_CLS(name) ((id)g_mac.get_class(name))
#define TW_SEL(name) (g_mac.sel(name))

static int tw_mac_load(void) {
    if (g_mac.loaded) return g_mac.loaded > 0;
    g_mac.loaded = -1;
    g_mac.objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    g_mac.appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit",
                          RTLD_NOW | RTLD_GLOBAL);
    if (!g_mac.objc || !g_mac.appkit) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot load AppKit: %s", dlerror());
        return 0;
    }
    g_mac.get_class = (Class (*)(const char*))dlsym(g_mac.objc, "objc_getClass");
    g_mac.sel = (SEL (*)(const char*))dlsym(g_mac.objc, "sel_registerName");
    g_mac.send = dlsym(g_mac.objc, "objc_msgSend");
#if defined(__x86_64__)
    g_mac.send_stret = dlsym(g_mac.objc, "objc_msgSend_stret");
#endif
    g_mac.pool_push = (void* (*)(void))dlsym(g_mac.objc, "objc_autoreleasePoolPush");
    g_mac.pool_pop = (void (*)(void*))dlsym(g_mac.objc, "objc_autoreleasePoolPop");
    g_mac.default_mode = (id*)dlsym(g_mac.appkit, "NSDefaultRunLoopMode");
    if (!g_mac.get_class || !g_mac.sel || !g_mac.send || !g_mac.pool_push ||
        !g_mac.pool_pop || !g_mac.default_mode
#if defined(__x86_64__)
        || !g_mac.send_stret
#endif
        ) {
        tw_fail(TW_ERR_UNAVAILABLE, "the Objective-C runtime is missing an entry point");
        return 0;
    }
    if (!g_mac.get_class("NSApplication")) {
        tw_fail(TW_ERR_UNAVAILABLE, "AppKit loaded without NSApplication");
        return 0;
    }
    g_mac.loaded = 1;
    return 1;
}

/* A method returning an NSRect. On x86_64 a 32-byte struct comes back through
 * objc_msgSend_stret; arm64 returns it in registers through objc_msgSend. */
static TwRect tw_mac_rect(id obj, const char* selector) {
#if defined(__x86_64__)
    return ((TwRect (*)(id, SEL))g_mac.send_stret)(obj, TW_SEL(selector));
#else
    return TW_SEND(TwRect)(obj, TW_SEL(selector));
#endif
}

struct TwWindow {
    id  window;   /* NSWindow, owned: alloc'd here and released on destroy */
    id  view;     /* its content view, owned by the window */
    int width, height;
    int close_requested;
    int resized;
};

static void tw_mac_measure(TwWindow* w) {
    TwRect b = tw_mac_rect(w->view, "bounds");
    double scale = TW_SEND(double)(w->window, TW_SEL("backingScaleFactor"));
    if (scale <= 0.0) scale = 1.0;
    int nw = (int)(b.w * scale + 0.5), nh = (int)(b.h * scale + 0.5);
    if (nw != w->width || nh != w->height) {
        w->width = nw;
        w->height = nh;
        w->resized = 1;
    }
}

static void tw_mac_pump_events(void) {
    id distant = TW_SEND(id)(TW_CLS("NSDate"), TW_SEL("distantPast"));
    for (;;) {
        id ev = TW_SEND(id, unsigned long long, id, id, BOOL)(
            g_mac.app, TW_SEL("nextEventMatchingMask:untilDate:inMode:dequeue:"),
            ~0ull, distant, *g_mac.default_mode, (BOOL)1);
        if (!ev) break;
        TW_SEND(void, id)(g_mac.app, TW_SEL("sendEvent:"), ev);
    }
    TW_SEND(void)(g_mac.app, TW_SEL("updateWindows"));
}

int tw_available(void) {
    return tw_mac_load();
}

TwWindow* tw_create(const char* title, int width, int height) {
    tw_clear_error();
    if (width <= 0 || height <= 0) {
        tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
        return NULL;
    }
    if (!pthread_main_np()) {
        tw_fail(TW_ERR_ARG, "AppKit windows can only be made on the main thread");
        return NULL;
    }
    if (!tw_mac_load()) return NULL;

    TwWindow* w = (TwWindow*)calloc(1, sizeof(*w));
    if (!w) { tw_fail(TW_ERR_OOM, "out of memory"); return NULL; }

    void* pool = g_mac.pool_push();
    if (!g_mac.app) {
        g_mac.app = TW_SEND(id)(TW_CLS("NSApplication"), TW_SEL("sharedApplication"));
        /* NSApplicationActivationPolicyRegular: a process with windows and a
         * Dock icon, which is what a window needs to be shown at all. */
        TW_SEND(void, long)(g_mac.app, TW_SEL("setActivationPolicy:"), 0);
        TW_SEND(void)(g_mac.app, TW_SEL("finishLaunching"));
    }

    /* The content rect is in POINTS; the caller asked for pixels. */
    id screen = TW_SEND(id)(TW_CLS("NSScreen"), TW_SEL("mainScreen"));
    double scale = screen ? TW_SEND(double)(screen, TW_SEL("backingScaleFactor")) : 1.0;
    if (scale <= 0.0) scale = 1.0;
    TwRect rect = { 0.0, 0.0, width / scale, height / scale };

    /* Titled | closable | miniaturizable | resizable; buffered backing. */
    const unsigned long style = 1ul | 2ul | 4ul | 8ul;
    id win = TW_SEND(id)(TW_CLS("NSWindow"), TW_SEL("alloc"));
    win = TW_SEND(id, TwRect, unsigned long, unsigned long, BOOL)(
        win, TW_SEL("initWithContentRect:styleMask:backing:defer:"),
        rect, style, 2ul, (BOOL)0);
    if (!win) {
        g_mac.pool_pop(pool);
        free(w);
        tw_fail(TW_ERR_SYSTEM, "NSWindow refused initWithContentRect");
        return NULL;
    }
    /* Released by tw_destroy, not by AppKit when the user closes it: the
     * handle stays valid until the caller is done with its swapchain. */
    TW_SEND(void, BOOL)(win, TW_SEL("setReleasedWhenClosed:"), (BOOL)0);
    id ns_title = TW_SEND(id, const char*)(TW_CLS("NSString"),
                                             TW_SEL("stringWithUTF8String:"),
                                             title ? title : "");
    TW_SEND(void, id)(win, TW_SEL("setTitle:"), ns_title);
    TW_SEND(void)(win, TW_SEL("center"));
    TW_SEND(void, id)(win, TW_SEL("makeKeyAndOrderFront:"), (id)0);
    TW_SEND(void, BOOL)(g_mac.app, TW_SEL("activateIgnoringOtherApps:"), (BOOL)1);

    w->window = win;
    w->view = TW_SEND(id)(win, TW_SEL("contentView"));
    tw_mac_pump_events();
    tw_mac_measure(w);
    w->resized = 0;
    g_mac.pool_pop(pool);
    return w;
}

void tw_destroy(TwWindow* w) {
    if (!w) return;
    if (w->window && g_mac.loaded > 0) {
        void* pool = g_mac.pool_push();
        TW_SEND(void)(w->window, TW_SEL("close"));
        TW_SEND(void)(w->window, TW_SEL("release"));
        tw_mac_pump_events();
        g_mac.pool_pop(pool);
    }
    free(w);
}

int tw_pump(TwWindow* w) {
    if (!w || g_mac.loaded <= 0) return 0;
    void* pool = g_mac.pool_push();
    tw_mac_pump_events();
    /* Closing a window that is not released on close orders it out, so a
     * window that stopped being visible is one the user closed. */
    if (!TW_SEND(BOOL)(w->window, TW_SEL("isVisible"))) w->close_requested = 1;
    tw_mac_measure(w);
    g_mac.pool_pop(pool);
    return w->close_requested ? 0 : 1;
}

int tw_set_size(TwWindow* w, int width, int height) {
    tw_clear_error();
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    if (width <= 0 || height <= 0) {
        return tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
    }
    void* pool = g_mac.pool_push();
    double scale = TW_SEND(double)(w->window, TW_SEL("backingScaleFactor"));
    if (scale <= 0.0) scale = 1.0;
    TwSize size = { width / scale, height / scale };
    TW_SEND(void, TwSize)(w->window, TW_SEL("setContentSize:"), size);
    tw_mac_pump_events();
    tw_mac_measure(w);
    g_mac.pool_pop(pool);
    return TW_OK;
}

int tw_set_minimized(TwWindow* w, int on) {
    (void)w; (void)on;
    return tw_fail(TW_ERR_UNSUPPORTED, "a miniaturised AppKit window keeps its size");
}

int   tw_kind(const TwWindow* w)    { return w ? TW_KIND_NSVIEW : TW_KIND_NONE; }
void* tw_handle(const TwWindow* w)  { return w ? (void*)w->view : NULL; }
void* tw_display(const TwWindow* w) { (void)w; return NULL; }

int tw_pixel(TwWindow* w, int x, int y) {
    (void)w; (void)x; (void)y;
    tw_fail(TW_ERR_UNSUPPORTED,
               "reading the screen on macOS needs the screen-recording permission");
    return -1;
}

/* ======================================================================== */
/* Linux and the BSDs: X11, opened at runtime                                */
/* ======================================================================== */
#else

#if defined(__has_include)
#  if __has_include(<X11/Xlib.h>) && __has_include(<X11/Xutil.h>)
#    define TW_HAVE_X11 1
#  endif
#endif

#if defined(TW_HAVE_X11)

#include <dlfcn.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* The libX11 entry points this module calls, fetched with dlsym so the build
 * needs only the headers and the binary starts where there is no libX11. */
#define TW_X11_FNS(X)                                                              \
    X(XOpenDisplay, Display*, (const char*))                                       \
    X(XCloseDisplay, int, (Display*))                                              \
    X(XDefaultScreen, int, (Display*))                                             \
    X(XRootWindow, Window, (Display*, int))                                        \
    X(XBlackPixel, unsigned long, (Display*, int))                                 \
    X(XCreateSimpleWindow, Window, (Display*, Window, int, int, unsigned int,      \
                                    unsigned int, unsigned int, unsigned long,     \
                                    unsigned long))                                \
    X(XDestroyWindow, int, (Display*, Window))                                     \
    X(XMapWindow, int, (Display*, Window))                                         \
    X(XStoreName, int, (Display*, Window, const char*))                            \
    X(XSelectInput, int, (Display*, Window, long))                                 \
    X(XInternAtom, Atom, (Display*, const char*, Bool))                            \
    X(XSetWMProtocols, Status, (Display*, Window, Atom*, int))                     \
    X(XSetWindowBackgroundPixmap, int, (Display*, Window, Pixmap))                 \
    X(XPending, int, (Display*))                                                   \
    X(XNextEvent, int, (Display*, XEvent*))                                        \
    X(XResizeWindow, int, (Display*, Window, unsigned int, unsigned int))          \
    X(XSync, int, (Display*, Bool))                                                \
    X(XGetWindowAttributes, Status, (Display*, Window, XWindowAttributes*))        \
    X(XGetImage, XImage*, (Display*, Drawable, int, int, unsigned int,             \
                           unsigned int, unsigned long, int))

static struct {
    int   loaded;           /* 0 unprobed, 1 usable, -1 not */
    void* lib;
#define TW_X11_DECL(name, ret, args) ret (*name) args;
    TW_X11_FNS(TW_X11_DECL)
#undef TW_X11_DECL
} g_x11;

static int tw_x11_load(void) {
    if (g_x11.loaded) return g_x11.loaded > 0;
    g_x11.loaded = -1;
    g_x11.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!g_x11.lib) g_x11.lib = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_x11.lib) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot load libX11: %s", dlerror());
        return 0;
    }
#define TW_X11_LOAD(name, ret, args)                                          \
    g_x11.name = (ret (*) args)dlsym(g_x11.lib, #name);                          \
    if (!g_x11.name) {                                                           \
        tw_fail(TW_ERR_UNAVAILABLE, "libX11 has no %s", #name);            \
        return 0;                                                                \
    }
    TW_X11_FNS(TW_X11_LOAD)
#undef TW_X11_LOAD
    g_x11.loaded = 1;
    return 1;
}

struct TwWindow {
    Display* dpy;          /* one connection per window, owned */
    Window   win;
    Atom     wm_delete;
    int      width, height;
    int      mapped;
    int      close_requested;
    int      resized;
};

static void tw_x11_handle(TwWindow* w, const XEvent* ev) {
    switch (ev->type) {
        case MapNotify:
            w->mapped = 1;
            break;
        case ConfigureNotify:
            if (ev->xconfigure.width != w->width || ev->xconfigure.height != w->height) {
                w->width = ev->xconfigure.width;
                w->height = ev->xconfigure.height;
                w->resized = 1;
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == w->wm_delete) w->close_requested = 1;
            break;
        case DestroyNotify:
            w->close_requested = 1;
            break;
        default:
            break;
    }
}

static void tw_x11_drain(TwWindow* w) {
    while (g_x11.XPending(w->dpy) > 0) {
        XEvent ev;
        g_x11.XNextEvent(w->dpy, &ev);
        tw_x11_handle(w, &ev);
    }
}

static void tw_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int tw_available(void) {
    if (!tw_x11_load()) return 0;
    Display* d = g_x11.XOpenDisplay(NULL);
    if (!d) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot open the X display (DISPLAY=%s)",
                   getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return 0;
    }
    g_x11.XCloseDisplay(d);
    return 1;
}

TwWindow* tw_create(const char* title, int width, int height) {
    tw_clear_error();
    if (width <= 0 || height <= 0) {
        tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
        return NULL;
    }
    if (!tw_x11_load()) return NULL;

    TwWindow* w = (TwWindow*)calloc(1, sizeof(*w));
    if (!w) { tw_fail(TW_ERR_OOM, "out of memory"); return NULL; }
    w->dpy = g_x11.XOpenDisplay(NULL);
    if (!w->dpy) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot open the X display (DISPLAY=%s)",
                   getenv("DISPLAY") ? getenv("DISPLAY") : "");
        free(w);
        return NULL;
    }
    int screen = g_x11.XDefaultScreen(w->dpy);
    Window root = g_x11.XRootWindow(w->dpy, screen);
    unsigned long black = g_x11.XBlackPixel(w->dpy, screen);
    w->win = g_x11.XCreateSimpleWindow(w->dpy, root, 0, 0, (unsigned)width,
                                       (unsigned)height, 0, black, black);
    if (!w->win) {
        g_x11.XCloseDisplay(w->dpy);
        free(w);
        tw_fail(TW_ERR_SYSTEM, "XCreateSimpleWindow failed");
        return NULL;
    }
    /* No background: the server would otherwise repaint it over whatever
     * the GPU presented each time the window is exposed. */
    g_x11.XSetWindowBackgroundPixmap(w->dpy, w->win, None);
    g_x11.XSelectInput(w->dpy, w->win, StructureNotifyMask | ExposureMask);
    g_x11.XStoreName(w->dpy, w->win, title ? title : "");
    w->wm_delete = g_x11.XInternAtom(w->dpy, "WM_DELETE_WINDOW", False);
    g_x11.XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);
    w->width = width;
    w->height = height;
    g_x11.XMapWindow(w->dpy, w->win);
    g_x11.XSync(w->dpy, False);

    /* Wait for the map, so the window is viewable before anything presents
     * into it. Bounded: a server that never maps it is reported, not waited
     * on forever. */
    for (int waited = 0; !w->mapped && waited < 2000; waited += 5) {
        tw_x11_drain(w);
        if (!w->mapped) tw_sleep_ms(5);
    }
    if (!w->mapped) {
        tw_destroy(w);
        tw_fail(TW_ERR_SYSTEM, "the X server did not map the window within 2 s");
        return NULL;
    }
    XWindowAttributes attrs;
    if (g_x11.XGetWindowAttributes(w->dpy, w->win, &attrs)) {
        w->width = attrs.width;
        w->height = attrs.height;
    }
    w->resized = 0;
    return w;
}

void tw_destroy(TwWindow* w) {
    if (!w) return;
    if (w->dpy) {
        if (w->win) g_x11.XDestroyWindow(w->dpy, w->win);
        g_x11.XCloseDisplay(w->dpy);
    }
    free(w);
}

int tw_pump(TwWindow* w) {
    if (!w || !w->dpy) return 0;
    tw_x11_drain(w);
    return w->close_requested ? 0 : 1;
}

int tw_set_size(TwWindow* w, int width, int height) {
    tw_clear_error();
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    if (width <= 0 || height <= 0) {
        return tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
    }
    g_x11.XResizeWindow(w->dpy, w->win, (unsigned)width, (unsigned)height);
    g_x11.XSync(w->dpy, False);
    /* The ConfigureNotify carries the size the server (or a window manager)
     * actually granted; wait for one, bounded. */
    for (int waited = 0; waited < 1000; waited += 5) {
        tw_x11_drain(w);
        if (w->width == width && w->height == height) break;
        tw_sleep_ms(5);
    }
    XWindowAttributes attrs;
    if (g_x11.XGetWindowAttributes(w->dpy, w->win, &attrs) &&
        (attrs.width != w->width || attrs.height != w->height)) {
        w->width = attrs.width;
        w->height = attrs.height;
        w->resized = 1;
    }
    return TW_OK;
}

int tw_set_minimized(TwWindow* w, int on) {
    (void)w; (void)on;
    return tw_fail(TW_ERR_UNSUPPORTED, "an iconified X11 window keeps its size");
}

int   tw_kind(const TwWindow* w)    { return w ? TW_KIND_X11 : TW_KIND_NONE; }
void* tw_handle(const TwWindow* w)  { return w ? (void*)(uintptr_t)w->win : NULL; }
void* tw_display(const TwWindow* w) { return w ? (void*)w->dpy : NULL; }

/* The shift that brings a visual's channel mask down to bit 0, and the
 * channel's width, so any TrueColor depth packs to 8 bits a channel. */
static unsigned tw_channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    int shift = 0;
    while (!((mask >> shift) & 1ul)) shift++;
    unsigned long bits = mask >> shift;
    unsigned long v = (pixel & mask) >> shift;
    return (unsigned)((v * 255ul + bits / 2ul) / bits);
}

int tw_pixel(TwWindow* w, int x, int y) {
    tw_clear_error();
    if (!w) { tw_fail(TW_ERR_ARG, "window is null"); return -1; }
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) {
        tw_fail(TW_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, w->width, w->height);
        return -1;
    }
    g_x11.XSync(w->dpy, False);
    XImage* img = g_x11.XGetImage(w->dpy, w->win, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!img) { tw_fail(TW_ERR_SYSTEM, "XGetImage failed"); return -1; }
    unsigned long p = XGetPixel(img, 0, 0);
    unsigned r = tw_channel(p, img->red_mask);
    unsigned g = tw_channel(p, img->green_mask);
    unsigned b = tw_channel(p, img->blue_mask);
    XDestroyImage(img);
    return (int)((r << 16) | (g << 8) | b);
}

#else  /* no X11 headers at build time */

/* The fields the shared accessors below read; nothing ever creates one. */
struct TwWindow {
    int width, height;
    int resized;
};

static int tw_no_x11(void) {
    return tw_fail(TW_ERR_UNAVAILABLE,
                      "built without the X11 headers (install libx11-dev and rebuild)");
}

int tw_available(void) { tw_no_x11(); return 0; }
TwWindow* tw_create(const char* title, int width, int height) {
    (void)title; (void)width; (void)height;
    tw_no_x11();
    return NULL;
}
void  tw_destroy(TwWindow* w) { (void)w; }
int   tw_pump(TwWindow* w) { (void)w; return 0; }
int   tw_set_size(TwWindow* w, int width, int height) {
    (void)w; (void)width; (void)height;
    return tw_no_x11();
}
int   tw_set_minimized(TwWindow* w, int on) { (void)w; (void)on; return tw_no_x11(); }
int   tw_kind(const TwWindow* w)    { (void)w; return TW_KIND_NONE; }
void* tw_handle(const TwWindow* w)  { (void)w; return NULL; }
void* tw_display(const TwWindow* w) { (void)w; return NULL; }
int   tw_pixel(TwWindow* w, int x, int y) {
    (void)w; (void)x; (void)y;
    tw_no_x11();
    return -1;
}

#endif /* TW_HAVE_X11 */
#endif /* platform */

/* ======================================================================== */
/* Shared                                                                    */
/* ======================================================================== */

int tw_width(const TwWindow* w)  { return w ? w->width : 0; }
int tw_height(const TwWindow* w) { return w ? w->height : 0; }

int tw_take_resized(TwWindow* w) {
    if (!w) return 0;
    int r = w->resized;
    w->resized = 0;
    return r;
}

/* ------------------------------------------------------------------------ */
/* Aether-facing entry points: signatures match what aetherc emits for the  */
/* externs a test declares (ptr is void*, string is const char*).           */
/* ------------------------------------------------------------------------ */

int         tw_ae_available(void)            { return tw_available(); }
const char* tw_ae_last_error(void)           { return tw_last_error(); }
void*       tw_ae_create(const char* t, int w, int h) { return (void*)tw_create(t, w, h); }
void        tw_ae_destroy(void* w)           { tw_destroy((TwWindow*)w); }
int         tw_ae_pump(void* w)              { return tw_pump((TwWindow*)w); }
int         tw_ae_width(void* w)             { return tw_width((const TwWindow*)w); }
int         tw_ae_height(void* w)            { return tw_height((const TwWindow*)w); }
int         tw_ae_take_resized(void* w)      { return tw_take_resized((TwWindow*)w); }
int         tw_ae_set_size(void* w, int width, int height) {
    return tw_set_size((TwWindow*)w, width, height);
}
int         tw_ae_set_minimized(void* w, int on) { return tw_set_minimized((TwWindow*)w, on); }
int         tw_ae_kind(void* w)              { return tw_kind((const TwWindow*)w); }
void*       tw_ae_handle(void* w)            { return tw_handle((const TwWindow*)w); }
void*       tw_ae_display(void* w)           { return tw_display((const TwWindow*)w); }
int         tw_ae_pixel(void* w, int x, int y) { return tw_pixel((TwWindow*)w, x, y); }
