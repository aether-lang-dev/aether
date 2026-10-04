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
/* Linux and the BSDs: X11 or Wayland, each opened at runtime                */
/* ======================================================================== */
#else

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<X11/Xlib.h>) && __has_include(<X11/Xutil.h>)
#    define TW_HAVE_X11 1
#  endif
#  if __has_include(<wayland-client-core.h>)
#    define TW_HAVE_WAYLAND 1
#  endif
#endif

/* One window struct for both backends: the shared accessors at the bottom
 * read width, height and resized, and `kind` says which backend's fields
 * below are live. */
#if defined(TW_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#if defined(TW_HAVE_WAYLAND)
#include <wayland-client-core.h>
#endif

struct TwWindow {
    int kind;
    int width, height;
    int resized;
    int close_requested;
#if defined(TW_HAVE_X11)
    struct {
        Display* dpy;          /* one connection per window, owned */
        Window   win;
        Atom     wm_delete;
        int      mapped;
    } x;
#endif
#if defined(TW_HAVE_WAYLAND)
    struct {
        struct wl_display*  display;   /* one connection per window, owned */
        struct wl_proxy*    registry;
        struct wl_proxy*    compositor;
        struct wl_proxy*    wm_base;
        struct wl_proxy*    surface;
        struct wl_proxy*    xdg_surface;
        struct wl_proxy*    toplevel;
        int                 configured;
        /* Reading the screen back (#2389): weston's weston_capture_v1, which
         * weston offers when started with --debug, captures an output into
         * a wl_shm buffer. `out_w` x `out_h` and `out_format` (a DRM fourcc)
         * are what the capture source asks for; `shot` is the mapped buffer
         * of the last capture. `capture_state` is 0 while one is in flight,
         * then 1 complete, 2 retry, 3 failed. */
        struct wl_proxy*    output;
        struct wl_proxy*    shm;
        struct wl_proxy*    capture;
        struct wl_proxy*    capture_source;
        struct wl_proxy*    shot_buffer;
        unsigned char*      shot;
        size_t              shot_bytes;
        int                 out_w, out_h;
        int                 shot_w, shot_h;
        uint32_t            out_format;
        int                 capture_state;
    } w;
#endif
};

static void tw_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Which window system to use, from AETHER_TEST_WINDOW_SYSTEM: "x11" or
 * "wayland". Unset, X11 comes first, because its screen can be read back
 * and Xwayland makes it reachable on a Wayland desktop too; Wayland is tried
 * when X11 is not there. The Wayland CI leg sets it to run the wl_surface
 * path on purpose. */
static const char* tw_requested_system(void) {
    const char* s = getenv("AETHER_TEST_WINDOW_SYSTEM");
    return s && s[0] ? s : "";
}

/* ------------------------------------------------------------------------ */
/* X11                                                                       */
/* ------------------------------------------------------------------------ */
#if defined(TW_HAVE_X11)

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

static void tw_x11_handle(TwWindow* w, const XEvent* ev) {
    switch (ev->type) {
        case MapNotify:
            w->x.mapped = 1;
            break;
        case ConfigureNotify:
            if (ev->xconfigure.width != w->width || ev->xconfigure.height != w->height) {
                w->width = ev->xconfigure.width;
                w->height = ev->xconfigure.height;
                w->resized = 1;
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == w->x.wm_delete) w->close_requested = 1;
            break;
        case DestroyNotify:
            w->close_requested = 1;
            break;
        default:
            break;
    }
}

static void tw_x11_drain(TwWindow* w) {
    while (g_x11.XPending(w->x.dpy) > 0) {
        XEvent ev;
        g_x11.XNextEvent(w->x.dpy, &ev);
        tw_x11_handle(w, &ev);
    }
}

static int tw_x11_available(void) {
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

static void tw_x11_destroy(TwWindow* w) {
    if (w->x.dpy) {
        if (w->x.win) g_x11.XDestroyWindow(w->x.dpy, w->x.win);
        g_x11.XCloseDisplay(w->x.dpy);
    }
    free(w);
}

static TwWindow* tw_x11_create(const char* title, int width, int height) {
    if (!tw_x11_load()) return NULL;

    TwWindow* w = (TwWindow*)calloc(1, sizeof(*w));
    if (!w) { tw_fail(TW_ERR_OOM, "out of memory"); return NULL; }
    w->kind = TW_KIND_X11;
    w->x.dpy = g_x11.XOpenDisplay(NULL);
    if (!w->x.dpy) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot open the X display (DISPLAY=%s)",
                   getenv("DISPLAY") ? getenv("DISPLAY") : "");
        free(w);
        return NULL;
    }
    int screen = g_x11.XDefaultScreen(w->x.dpy);
    Window root = g_x11.XRootWindow(w->x.dpy, screen);
    unsigned long black = g_x11.XBlackPixel(w->x.dpy, screen);
    w->x.win = g_x11.XCreateSimpleWindow(w->x.dpy, root, 0, 0, (unsigned)width,
                                         (unsigned)height, 0, black, black);
    if (!w->x.win) {
        g_x11.XCloseDisplay(w->x.dpy);
        free(w);
        tw_fail(TW_ERR_SYSTEM, "XCreateSimpleWindow failed");
        return NULL;
    }
    /* No background: the server would otherwise repaint it over whatever
     * the GPU presented each time the window is exposed. */
    g_x11.XSetWindowBackgroundPixmap(w->x.dpy, w->x.win, None);
    g_x11.XSelectInput(w->x.dpy, w->x.win, StructureNotifyMask | ExposureMask);
    g_x11.XStoreName(w->x.dpy, w->x.win, title ? title : "");
    w->x.wm_delete = g_x11.XInternAtom(w->x.dpy, "WM_DELETE_WINDOW", False);
    g_x11.XSetWMProtocols(w->x.dpy, w->x.win, &w->x.wm_delete, 1);
    w->width = width;
    w->height = height;
    g_x11.XMapWindow(w->x.dpy, w->x.win);
    g_x11.XSync(w->x.dpy, False);

    /* Wait for the map, so the window is viewable before anything presents
     * into it. Bounded: a server that never maps it is reported, not waited
     * on forever. */
    for (int waited = 0; !w->x.mapped && waited < 2000; waited += 5) {
        tw_x11_drain(w);
        if (!w->x.mapped) tw_sleep_ms(5);
    }
    if (!w->x.mapped) {
        tw_x11_destroy(w);
        tw_fail(TW_ERR_SYSTEM, "the X server did not map the window within 2 s");
        return NULL;
    }
    XWindowAttributes attrs;
    if (g_x11.XGetWindowAttributes(w->x.dpy, w->x.win, &attrs)) {
        w->width = attrs.width;
        w->height = attrs.height;
    }
    w->resized = 0;
    return w;
}

static int tw_x11_pump(TwWindow* w) {
    if (!w->x.dpy) return 0;
    tw_x11_drain(w);
    return w->close_requested ? 0 : 1;
}

static int tw_x11_set_size(TwWindow* w, int width, int height) {
    g_x11.XResizeWindow(w->x.dpy, w->x.win, (unsigned)width, (unsigned)height);
    g_x11.XSync(w->x.dpy, False);
    /* The ConfigureNotify carries the size the server (or a window manager)
     * actually granted; wait for one, bounded. */
    for (int waited = 0; waited < 1000; waited += 5) {
        tw_x11_drain(w);
        if (w->width == width && w->height == height) break;
        tw_sleep_ms(5);
    }
    XWindowAttributes attrs;
    if (g_x11.XGetWindowAttributes(w->x.dpy, w->x.win, &attrs) &&
        (attrs.width != w->width || attrs.height != w->height)) {
        w->width = attrs.width;
        w->height = attrs.height;
        w->resized = 1;
    }
    return TW_OK;
}

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

static int tw_x11_pixel(TwWindow* w, int x, int y) {
    g_x11.XSync(w->x.dpy, False);
    XImage* img = g_x11.XGetImage(w->x.dpy, w->x.win, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!img) { tw_fail(TW_ERR_SYSTEM, "XGetImage failed"); return -1; }
    unsigned long p = XGetPixel(img, 0, 0);
    unsigned r = tw_channel(p, img->red_mask);
    unsigned g = tw_channel(p, img->green_mask);
    unsigned b = tw_channel(p, img->blue_mask);
    XDestroyImage(img);
    return (int)((r << 16) | (g << 8) | b);
}

#endif /* TW_HAVE_X11 */

/* ------------------------------------------------------------------------ */
/* Wayland (#2197): wl_compositor + xdg_wm_base through libwayland-client   */
/* ------------------------------------------------------------------------ */
#if defined(TW_HAVE_WAYLAND)

/* The libwayland-client entry points this module calls, fetched with dlsym
 * like libX11's, plus the four core interfaces it exports: the fixture
 * links nothing and starts where there is no libwayland. */
#define TW_WL_FNS(X)                                                                   \
    X(wl_display_connect, struct wl_display*, (const char*))                           \
    X(wl_display_disconnect, void, (struct wl_display*))                               \
    X(wl_display_roundtrip, int, (struct wl_display*))                                 \
    X(wl_display_flush, int, (struct wl_display*))                                     \
    X(wl_display_get_fd, int, (struct wl_display*))                                    \
    X(wl_display_dispatch_pending, int, (struct wl_display*))                          \
    X(wl_display_prepare_read, int, (struct wl_display*))                              \
    X(wl_display_read_events, int, (struct wl_display*))                               \
    X(wl_display_cancel_read, void, (struct wl_display*))                              \
    X(wl_proxy_marshal, void, (struct wl_proxy*, uint32_t, ...))                       \
    X(wl_proxy_marshal_constructor, struct wl_proxy*,                                  \
      (struct wl_proxy*, uint32_t, const struct wl_interface*, ...))                   \
    X(wl_proxy_marshal_constructor_versioned, struct wl_proxy*,                        \
      (struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, ...))         \
    X(wl_proxy_add_listener, int, (struct wl_proxy*, void (**)(void), void*))          \
    X(wl_proxy_destroy, void, (struct wl_proxy*))

static struct {
    int   loaded;           /* 0 unprobed, 1 usable, -1 not */
    void* lib;
#define TW_WL_DECL(name, ret, args) ret (*name) args;
    TW_WL_FNS(TW_WL_DECL)
#undef TW_WL_DECL
    const struct wl_interface* registry;
    const struct wl_interface* compositor;
    const struct wl_interface* surface;
    const struct wl_interface* output;
    const struct wl_interface* shm;
    const struct wl_interface* shm_pool;
    const struct wl_interface* buffer;
} g_wl;

/* xdg-shell, the part a top-level window needs, spelled as wayland-scanner
 * would generate it from xdg-shell.xml (stable, version 1). Only the
 * requests this fixture sends carry argument types; the rest keep their
 * signature so the opcodes line up, with no types, since they are never
 * marshalled. xdg_wm_base is bound at version 1, so the compositor sends
 * only the events listed here. */
static const struct wl_interface* tw_no_types[8];
static const struct wl_interface tw_xdg_surface_interface;
static const struct wl_interface tw_xdg_toplevel_interface;

static const struct wl_interface* tw_get_xdg_surface_types[2] = { &tw_xdg_surface_interface, NULL };
static const struct wl_interface* tw_get_toplevel_types[1]    = { &tw_xdg_toplevel_interface };

static const struct wl_message tw_xdg_wm_base_requests[] = {
    { "destroy",           "",   tw_no_types },
    { "create_positioner", "n",  tw_no_types },
    { "get_xdg_surface",   "no", tw_get_xdg_surface_types },
    { "pong",              "u",  tw_no_types },
};
static const struct wl_message tw_xdg_wm_base_events[] = {
    { "ping", "u", tw_no_types },
};
static const struct wl_interface tw_xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, tw_xdg_wm_base_requests, 1, tw_xdg_wm_base_events,
};

static const struct wl_message tw_xdg_surface_requests[] = {
    { "destroy",             "",     tw_no_types },
    { "get_toplevel",        "n",    tw_get_toplevel_types },
    { "get_popup",           "n?oo", tw_no_types },
    { "set_window_geometry", "iiii", tw_no_types },
    { "ack_configure",       "u",    tw_no_types },
};
static const struct wl_message tw_xdg_surface_events[] = {
    { "configure", "u", tw_no_types },
};
static const struct wl_interface tw_xdg_surface_interface = {
    "xdg_surface", 1, 5, tw_xdg_surface_requests, 1, tw_xdg_surface_events,
};

static const struct wl_message tw_xdg_toplevel_requests[] = {
    { "destroy",          "",     tw_no_types },
    { "set_parent",       "?o",   tw_no_types },
    { "set_title",        "s",    tw_no_types },
    { "set_app_id",       "s",    tw_no_types },
    { "show_window_menu", "ouii", tw_no_types },
    { "move",             "ou",   tw_no_types },
    { "resize",           "ouu",  tw_no_types },
    { "set_max_size",     "ii",   tw_no_types },
    { "set_min_size",     "ii",   tw_no_types },
    { "set_maximized",    "",     tw_no_types },
    { "unset_maximized",  "",     tw_no_types },
    { "set_fullscreen",   "?o",   tw_no_types },
    { "unset_fullscreen", "",     tw_no_types },
    { "set_minimized",    "",     tw_no_types },
};
static const struct wl_message tw_xdg_toplevel_events[] = {
    { "configure", "iia", tw_no_types },
    { "close",     "",    tw_no_types },
};
static const struct wl_interface tw_xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, tw_xdg_toplevel_requests, 2, tw_xdg_toplevel_events,
};

/* weston-output-capture (weston 12 and later, #2389), spelled the same way
 * from weston-output-capture.xml, version 1: weston_capture_v1.create takes
 * a wl_output and a pixel source and makes a weston_capture_source_v1,
 * whose capture request fills a wl_buffer and answers with complete, retry
 * or failed. */
static const struct wl_interface tw_capture_source_interface;
static const struct wl_interface* tw_capture_create_types[3] = { NULL, NULL, &tw_capture_source_interface };
static const struct wl_interface* tw_capture_buffer_types[1] = { NULL };

static const struct wl_message tw_capture_requests[] = {
    { "destroy", "",    tw_no_types },
    { "create",  "oun", tw_capture_create_types },
};
static const struct wl_interface tw_capture_interface = {
    "weston_capture_v1", 1, 2, tw_capture_requests, 0, NULL,
};

static const struct wl_message tw_capture_source_requests[] = {
    { "destroy", "",  tw_no_types },
    { "capture", "o", tw_capture_buffer_types },
};
static const struct wl_message tw_capture_source_events[] = {
    { "format",   "u",  tw_no_types },
    { "size",     "ii", tw_no_types },
    { "complete", "",   tw_no_types },
    { "retry",    "",   tw_no_types },
    { "failed",   "?s", tw_no_types },
};
static const struct wl_interface tw_capture_source_interface = {
    "weston_capture_source_v1", 1, 2, tw_capture_source_requests, 5, tw_capture_source_events,
};

/* Opcodes of the core requests this fixture sends. */
#define TW_WL_DISPLAY_GET_REGISTRY      1
#define TW_WL_REGISTRY_BIND             0
#define TW_WL_COMPOSITOR_CREATE_SURFACE 0
#define TW_WL_SURFACE_DESTROY           0
#define TW_WL_SURFACE_COMMIT            6
#define TW_XDG_WM_BASE_DESTROY          0
#define TW_XDG_WM_BASE_GET_XDG_SURFACE  2
#define TW_XDG_WM_BASE_PONG             3
#define TW_XDG_SURFACE_DESTROY          0
#define TW_XDG_SURFACE_GET_TOPLEVEL     1
#define TW_XDG_SURFACE_ACK_CONFIGURE    4
#define TW_XDG_TOPLEVEL_DESTROY         0
#define TW_XDG_TOPLEVEL_SET_TITLE       2
#define TW_XDG_TOPLEVEL_SET_APP_ID      3
#define TW_WL_SHM_CREATE_POOL           0
#define TW_WL_SHM_POOL_CREATE_BUFFER    0
#define TW_WL_SHM_POOL_DESTROY          1
#define TW_WL_BUFFER_DESTROY            0
#define TW_CAPTURE_DESTROY              0
#define TW_CAPTURE_CREATE               1
#define TW_CAPTURE_SOURCE_DESTROY       0
#define TW_CAPTURE_SOURCE_CAPTURE       1
#define TW_CAPTURE_SOURCE_FRAMEBUFFER   1   /* weston_capture_v1.source */
/* DRM fourcc codes, and wl_shm's own numbers for the two it renames. */
#define TW_DRM_ARGB8888                 0x34325241u   /* 'AR24' */
#define TW_DRM_XRGB8888                 0x34325258u   /* 'XR24' */

static int tw_wl_load(void) {
    if (g_wl.loaded) return g_wl.loaded > 0;
    g_wl.loaded = -1;
    g_wl.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_wl.lib) g_wl.lib = dlopen("libwayland-client.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_wl.lib) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot load libwayland-client: %s", dlerror());
        return 0;
    }
#define TW_WL_LOAD(name, ret, args)                                              \
    g_wl.name = (ret (*) args)dlsym(g_wl.lib, #name);                              \
    if (!g_wl.name) {                                                               \
        tw_fail(TW_ERR_UNAVAILABLE, "libwayland-client has no %s", #name);       \
        return 0;                                                                   \
    }
    TW_WL_FNS(TW_WL_LOAD)
#undef TW_WL_LOAD
    g_wl.registry   = (const struct wl_interface*)dlsym(g_wl.lib, "wl_registry_interface");
    g_wl.compositor = (const struct wl_interface*)dlsym(g_wl.lib, "wl_compositor_interface");
    g_wl.surface    = (const struct wl_interface*)dlsym(g_wl.lib, "wl_surface_interface");
    g_wl.output     = (const struct wl_interface*)dlsym(g_wl.lib, "wl_output_interface");
    g_wl.shm        = (const struct wl_interface*)dlsym(g_wl.lib, "wl_shm_interface");
    g_wl.shm_pool   = (const struct wl_interface*)dlsym(g_wl.lib, "wl_shm_pool_interface");
    g_wl.buffer     = (const struct wl_interface*)dlsym(g_wl.lib, "wl_buffer_interface");
    if (!g_wl.registry || !g_wl.compositor || !g_wl.surface || !g_wl.output || !g_wl.shm ||
        !g_wl.shm_pool || !g_wl.buffer) {
        tw_fail(TW_ERR_UNAVAILABLE, "libwayland-client exports no core interfaces");
        return 0;
    }
    tw_get_xdg_surface_types[1] = g_wl.surface;
    tw_capture_create_types[0] = g_wl.output;
    tw_capture_buffer_types[0] = g_wl.buffer;
    g_wl.loaded = 1;
    return 1;
}

/* Listeners. Each proxy carries its TwWindow as user data. */
static void tw_wl_on_global(void* data, struct wl_proxy* registry, uint32_t name,
                            const char* interface, uint32_t version) {
    TwWindow* w = (TwWindow*)data;
    if (strcmp(interface, "wl_compositor") == 0 && !w->w.compositor) {
        uint32_t v = version < 4 ? version : 4;
        w->w.compositor = g_wl.wl_proxy_marshal_constructor_versioned(
            registry, TW_WL_REGISTRY_BIND, g_wl.compositor, v, name, g_wl.compositor->name, v, NULL);
    } else if (strcmp(interface, "xdg_wm_base") == 0 && !w->w.wm_base) {
        w->w.wm_base = g_wl.wl_proxy_marshal_constructor_versioned(
            registry, TW_WL_REGISTRY_BIND, &tw_xdg_wm_base_interface, 1, name, "xdg_wm_base", 1, NULL);
    } else if (strcmp(interface, "wl_output") == 0 && !w->w.output) {
        /* The first output: weston's headless backend has the one. */
        w->w.output = g_wl.wl_proxy_marshal_constructor_versioned(
            registry, TW_WL_REGISTRY_BIND, g_wl.output, 1, name, g_wl.output->name, 1, NULL);
    } else if (strcmp(interface, "wl_shm") == 0 && !w->w.shm) {
        w->w.shm = g_wl.wl_proxy_marshal_constructor_versioned(
            registry, TW_WL_REGISTRY_BIND, g_wl.shm, 1, name, g_wl.shm->name, 1, NULL);
    } else if (strcmp(interface, "weston_capture_v1") == 0 && !w->w.capture) {
        w->w.capture = g_wl.wl_proxy_marshal_constructor_versioned(
            registry, TW_WL_REGISTRY_BIND, &tw_capture_interface, 1, name, "weston_capture_v1", 1, NULL);
    }
}
static void tw_wl_on_global_remove(void* data, struct wl_proxy* registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static void (*tw_wl_registry_listener[])(void) = {
    (void (*)(void))tw_wl_on_global,
    (void (*)(void))tw_wl_on_global_remove,
};

static void tw_wl_on_ping(void* data, struct wl_proxy* wm_base, uint32_t serial) {
    (void)data;
    g_wl.wl_proxy_marshal(wm_base, TW_XDG_WM_BASE_PONG, serial);
}
static void (*tw_wl_wm_base_listener[])(void) = { (void (*)(void))tw_wl_on_ping };

static void tw_wl_on_configure(void* data, struct wl_proxy* xdg_surface, uint32_t serial) {
    TwWindow* w = (TwWindow*)data;
    g_wl.wl_proxy_marshal(xdg_surface, TW_XDG_SURFACE_ACK_CONFIGURE, serial);
    w->w.configured = 1;
}
static void (*tw_wl_xdg_surface_listener[])(void) = { (void (*)(void))tw_wl_on_configure };

/* A compositor that wants the window at a size says so here; 0 x 0 leaves
 * the size to the client, which keeps what it asked for. */
static void tw_wl_on_toplevel_configure(void* data, struct wl_proxy* toplevel,
                                        int32_t width, int32_t height, struct wl_array* states) {
    (void)toplevel; (void)states;
    TwWindow* w = (TwWindow*)data;
    if (width > 0 && height > 0 && (width != w->width || height != w->height)) {
        w->width = width;
        w->height = height;
        w->resized = 1;
    }
}
static void tw_wl_on_toplevel_close(void* data, struct wl_proxy* toplevel) {
    (void)toplevel;
    ((TwWindow*)data)->close_requested = 1;
}
static void (*tw_wl_toplevel_listener[])(void) = {
    (void (*)(void))tw_wl_on_toplevel_configure,
    (void (*)(void))tw_wl_on_toplevel_close,
};

/* Handles what the compositor has sent without blocking: the read pattern
 * libwayland documents for a client that shares the connection with
 * another consumer (here the Vulkan WSI, on its own queue). */
static void tw_wl_drain(TwWindow* w) {
    struct wl_display* d = w->w.display;
    while (g_wl.wl_display_prepare_read(d) != 0) g_wl.wl_display_dispatch_pending(d);
    g_wl.wl_display_flush(d);
    struct pollfd pfd;
    pfd.fd = g_wl.wl_display_get_fd(d);
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) g_wl.wl_display_read_events(d);
    else g_wl.wl_display_cancel_read(d);
    g_wl.wl_display_dispatch_pending(d);
}

static int tw_wl_available(void) {
    if (!tw_wl_load()) return 0;
    struct wl_display* d = g_wl.wl_display_connect(NULL);
    if (!d) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot connect to the Wayland display (WAYLAND_DISPLAY=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        return 0;
    }
    g_wl.wl_display_disconnect(d);
    return 1;
}

static void tw_wl_free_shot(TwWindow* w) {
    if (w->w.shot_buffer) {
        g_wl.wl_proxy_marshal(w->w.shot_buffer, TW_WL_BUFFER_DESTROY);
        g_wl.wl_proxy_destroy(w->w.shot_buffer);
    }
    if (w->w.shot) munmap(w->w.shot, w->w.shot_bytes);
    w->w.shot_buffer = NULL;
    w->w.shot = NULL;
    w->w.shot_bytes = 0;
    w->w.shot_w = w->w.shot_h = 0;
}

static void tw_wl_destroy(TwWindow* w) {
    tw_wl_free_shot(w);
    if (w->w.capture_source) {
        g_wl.wl_proxy_marshal(w->w.capture_source, TW_CAPTURE_SOURCE_DESTROY);
        g_wl.wl_proxy_destroy(w->w.capture_source);
    }
    if (w->w.capture) { g_wl.wl_proxy_marshal(w->w.capture, TW_CAPTURE_DESTROY); g_wl.wl_proxy_destroy(w->w.capture); }
    if (w->w.shm)     g_wl.wl_proxy_destroy(w->w.shm);
    if (w->w.output)  g_wl.wl_proxy_destroy(w->w.output);
    if (w->w.toplevel)    { g_wl.wl_proxy_marshal(w->w.toplevel, TW_XDG_TOPLEVEL_DESTROY); g_wl.wl_proxy_destroy(w->w.toplevel); }
    if (w->w.xdg_surface) { g_wl.wl_proxy_marshal(w->w.xdg_surface, TW_XDG_SURFACE_DESTROY); g_wl.wl_proxy_destroy(w->w.xdg_surface); }
    if (w->w.surface)     { g_wl.wl_proxy_marshal(w->w.surface, TW_WL_SURFACE_DESTROY); g_wl.wl_proxy_destroy(w->w.surface); }
    if (w->w.wm_base)     { g_wl.wl_proxy_marshal(w->w.wm_base, TW_XDG_WM_BASE_DESTROY); g_wl.wl_proxy_destroy(w->w.wm_base); }
    if (w->w.compositor)  g_wl.wl_proxy_destroy(w->w.compositor);
    if (w->w.registry)    g_wl.wl_proxy_destroy(w->w.registry);
    if (w->w.display) {
        g_wl.wl_display_flush(w->w.display);
        g_wl.wl_display_disconnect(w->w.display);
    }
    free(w);
}

static TwWindow* tw_wl_create(const char* title, int width, int height) {
    if (!tw_wl_load()) return NULL;

    TwWindow* w = (TwWindow*)calloc(1, sizeof(*w));
    if (!w) { tw_fail(TW_ERR_OOM, "out of memory"); return NULL; }
    w->kind = TW_KIND_WAYLAND;
    w->width = width;
    w->height = height;
    w->w.display = g_wl.wl_display_connect(NULL);
    if (!w->w.display) {
        tw_fail(TW_ERR_UNAVAILABLE, "cannot connect to the Wayland display (WAYLAND_DISPLAY=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        free(w);
        return NULL;
    }
    w->w.registry = g_wl.wl_proxy_marshal_constructor((struct wl_proxy*)w->w.display,
                                                      TW_WL_DISPLAY_GET_REGISTRY, g_wl.registry, NULL);
    if (!w->w.registry) {
        tw_wl_destroy(w);
        tw_fail(TW_ERR_SYSTEM, "wl_display.get_registry failed");
        return NULL;
    }
    g_wl.wl_proxy_add_listener(w->w.registry, tw_wl_registry_listener, w);
    g_wl.wl_display_roundtrip(w->w.display);   /* the globals */
    if (!w->w.compositor || !w->w.wm_base) {
        tw_wl_destroy(w);
        tw_fail(TW_ERR_UNSUPPORTED, "the compositor offers no %s",
                !w->w.compositor ? "wl_compositor" : "xdg_wm_base");
        return NULL;
    }
    g_wl.wl_proxy_add_listener(w->w.wm_base, tw_wl_wm_base_listener, w);

    w->w.surface = g_wl.wl_proxy_marshal_constructor(w->w.compositor, TW_WL_COMPOSITOR_CREATE_SURFACE,
                                                     g_wl.surface, NULL);
    w->w.xdg_surface = w->w.surface
        ? g_wl.wl_proxy_marshal_constructor(w->w.wm_base, TW_XDG_WM_BASE_GET_XDG_SURFACE,
                                            &tw_xdg_surface_interface, NULL, w->w.surface)
        : NULL;
    w->w.toplevel = w->w.xdg_surface
        ? g_wl.wl_proxy_marshal_constructor(w->w.xdg_surface, TW_XDG_SURFACE_GET_TOPLEVEL,
                                            &tw_xdg_toplevel_interface, NULL)
        : NULL;
    if (!w->w.toplevel) {
        tw_wl_destroy(w);
        tw_fail(TW_ERR_SYSTEM, "cannot make an xdg_toplevel");
        return NULL;
    }
    g_wl.wl_proxy_add_listener(w->w.xdg_surface, tw_wl_xdg_surface_listener, w);
    g_wl.wl_proxy_add_listener(w->w.toplevel, tw_wl_toplevel_listener, w);
    g_wl.wl_proxy_marshal(w->w.toplevel, TW_XDG_TOPLEVEL_SET_TITLE, title ? title : "");
    g_wl.wl_proxy_marshal(w->w.toplevel, TW_XDG_TOPLEVEL_SET_APP_ID, "aether-contrib-test");
    /* The initial commit, with no buffer, asks for the first configure;
     * only after acknowledging it may anything attach a buffer, which is
     * what the Vulkan swapchain will do. */
    g_wl.wl_proxy_marshal(w->w.surface, TW_WL_SURFACE_COMMIT);
    g_wl.wl_display_flush(w->w.display);
    for (int waited = 0; !w->w.configured && waited < 2000; waited += 5) {
        tw_wl_drain(w);
        if (!w->w.configured) tw_sleep_ms(5);
    }
    if (!w->w.configured) {
        tw_wl_destroy(w);
        tw_fail(TW_ERR_SYSTEM, "the compositor did not configure the window within 2 s");
        return NULL;
    }
    w->resized = 0;
    return w;
}

static int tw_wl_pump(TwWindow* w) {
    if (!w->w.display) return 0;
    tw_wl_drain(w);
    return w->close_requested ? 0 : 1;
}

/* Under xdg-shell the client owns its size: the next buffer it attaches at
 * the new size is the resize, so the fixture only records what was asked. */
static int tw_wl_set_size(TwWindow* w, int width, int height) {
    if (width != w->width || height != w->height) {
        w->width = width;
        w->height = height;
        w->resized = 1;
    }
    tw_wl_drain(w);
    return TW_OK;
}

/* The capture source's events (#2389). */
static void tw_wl_on_capture_format(void* data, struct wl_proxy* src, uint32_t format) {
    (void)src;
    ((TwWindow*)data)->w.out_format = format;
}
static void tw_wl_on_capture_size(void* data, struct wl_proxy* src, int32_t width, int32_t height) {
    (void)src;
    TwWindow* w = (TwWindow*)data;
    w->w.out_w = width;
    w->w.out_h = height;
}
static void tw_wl_on_capture_complete(void* data, struct wl_proxy* src) {
    (void)src;
    ((TwWindow*)data)->w.capture_state = 1;
}
static void tw_wl_on_capture_retry(void* data, struct wl_proxy* src) {
    (void)src;
    ((TwWindow*)data)->w.capture_state = 2;
}
static void tw_wl_on_capture_failed(void* data, struct wl_proxy* src, const char* msg) {
    (void)src;
    TwWindow* w = (TwWindow*)data;
    w->w.capture_state = 3;
    tw_fail(TW_ERR_SYSTEM, "weston could not capture the output: %s", msg ? msg : "no reason given");
}
static void (*tw_wl_capture_source_listener[])(void) = {
    (void (*)(void))tw_wl_on_capture_format,
    (void (*)(void))tw_wl_on_capture_size,
    (void (*)(void))tw_wl_on_capture_complete,
    (void (*)(void))tw_wl_on_capture_retry,
    (void (*)(void))tw_wl_on_capture_failed,
};

/* Waits, up to `ms`, for the compositor's events to set *flag. */
static int tw_wl_wait(TwWindow* w, const int* flag, int ms) {
    for (int waited = 0; !*flag && waited < ms; waited += 5) {
        tw_wl_drain(w);
        if (!*flag) tw_sleep_ms(5);
    }
    return *flag;
}

/* A wl_shm buffer of the output's size and format, mapped, kept until the
 * capture source asks for another. */
static int tw_wl_make_shot(TwWindow* w) {
    tw_wl_free_shot(w);
    int stride = w->w.out_w * 4;
    size_t bytes = (size_t)stride * (size_t)w->w.out_h;
    char path[] = "/tmp/aether-tw-capture-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return tw_fail(TW_ERR_SYSTEM, "cannot make a file for the capture buffer");
    unlink(path);
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        return tw_fail(TW_ERR_SYSTEM, "cannot size the capture buffer");
    }
    void* map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return tw_fail(TW_ERR_SYSTEM, "cannot map the capture buffer");
    }
    /* wl_shm names ARGB8888 and XRGB8888 0 and 1; every other format is
     * its DRM fourcc. */
    uint32_t shm_format = w->w.out_format == TW_DRM_ARGB8888 ? 0u
                        : w->w.out_format == TW_DRM_XRGB8888 ? 1u : w->w.out_format;
    struct wl_proxy* pool = g_wl.wl_proxy_marshal_constructor(w->w.shm, TW_WL_SHM_CREATE_POOL, g_wl.shm_pool,
                                                              NULL, fd, (int32_t)bytes);
    struct wl_proxy* buffer = pool
        ? g_wl.wl_proxy_marshal_constructor(pool, TW_WL_SHM_POOL_CREATE_BUFFER, g_wl.buffer, NULL, 0,
                                            w->w.out_w, w->w.out_h, stride, shm_format)
        : NULL;
    if (pool) {
        g_wl.wl_proxy_marshal(pool, TW_WL_SHM_POOL_DESTROY);
        g_wl.wl_proxy_destroy(pool);
    }
    close(fd);
    if (!buffer) {
        munmap(map, bytes);
        return tw_fail(TW_ERR_SYSTEM, "cannot make the capture buffer");
    }
    w->w.shot = (unsigned char*)map;
    w->w.shot_bytes = bytes;
    w->w.shot_buffer = buffer;
    w->w.shot_w = w->w.out_w;
    w->w.shot_h = w->w.out_h;
    return TW_OK;
}

/* The colour on screen at client pixel (x, y): the output captured through
 * weston_capture_v1, read where the window is on it. The fixture is meant
 * for weston's kiosk shell, which shows the window fullscreen, centred on
 * the output when its buffer is smaller (weston_shell_utils_center_on_output);
 * that is where the client pixel is looked up. */
static int tw_wl_pixel(TwWindow* w, int x, int y) {
    if (!w->w.capture || !w->w.output || !w->w.shm) {
        tw_fail(TW_ERR_UNSUPPORTED, "the screen is not readable on Wayland: the compositor offers no %s "
                "(weston offers it with --debug)", !w->w.capture ? "weston_capture_v1" : "wl_output or wl_shm");
        return -1;
    }
    if (!w->w.capture_source) {
        w->w.capture_source = g_wl.wl_proxy_marshal_constructor(w->w.capture, TW_CAPTURE_CREATE,
                                                                &tw_capture_source_interface, w->w.output,
                                                                (uint32_t)TW_CAPTURE_SOURCE_FRAMEBUFFER, NULL);
        if (!w->w.capture_source) { tw_fail(TW_ERR_SYSTEM, "weston_capture_v1.create failed"); return -1; }
        g_wl.wl_proxy_add_listener(w->w.capture_source, tw_wl_capture_source_listener, w);
        g_wl.wl_display_roundtrip(w->w.display);   /* format and size */
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (w->w.out_w <= 0 || w->w.out_h <= 0) {
            tw_fail(TW_ERR_SYSTEM, "the capture source reported no output size");
            return -1;
        }
        if (w->w.out_format != TW_DRM_ARGB8888 && w->w.out_format != TW_DRM_XRGB8888) {
            tw_fail(TW_ERR_UNSUPPORTED, "the output captures as DRM format 0x%08x, not 8-bit RGB",
                    (unsigned)w->w.out_format);
            return -1;
        }
        if (!w->w.shot || w->w.shot_w != w->w.out_w || w->w.shot_h != w->w.out_h) {
            if (tw_wl_make_shot(w) != TW_OK) return -1;
        }
        w->w.capture_state = 0;
        g_wl.wl_proxy_marshal(w->w.capture_source, TW_CAPTURE_SOURCE_CAPTURE, w->w.shot_buffer);
        g_wl.wl_display_flush(w->w.display);
        if (!tw_wl_wait(w, &w->w.capture_state, 2000)) {
            tw_fail(TW_ERR_SYSTEM, "weston did not capture the output within 2 s");
            return -1;
        }
        if (w->w.capture_state == 1) break;
        if (w->w.capture_state == 3) return -1;
        /* retry: the size or format changed, and the next round reallocates. */
    }
    if (w->w.capture_state != 1) {
        tw_fail(TW_ERR_SYSTEM, "weston asked to retry the capture three times");
        return -1;
    }
    int ox = (w->w.out_w - w->width) / 2;
    int oy = (w->w.out_h - w->height) / 2;
    int sx = ox + x, sy = oy + y;
    if (sx < 0 || sy < 0 || sx >= w->w.out_w || sy >= w->w.out_h) {
        tw_fail(TW_ERR_ARG, "client pixel %d,%d is off the %dx%d output", x, y, w->w.out_w, w->w.out_h);
        return -1;
    }
    /* [AX]RGB8888 is a little-endian 32-bit word: blue, green, red, then
     * alpha or padding. */
    const unsigned char* p = w->w.shot + ((size_t)sy * (size_t)w->w.out_w + (size_t)sx) * 4u;
    return (int)(((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[0]);
}

#endif /* TW_HAVE_WAYLAND */

/* ------------------------------------------------------------------------ */
/* The public entry points, dispatching on the window system in use          */
/* ------------------------------------------------------------------------ */

#if defined(TW_HAVE_X11) || defined(TW_HAVE_WAYLAND)

/* The backend the environment asks for and this build has; TW_KIND_NONE
 * with the reason set when neither can open a window here. */
static int tw_pick_system(void) {
    const char* want = tw_requested_system();
    int want_x11 = strcmp(want, "x11") == 0;
    int want_wl  = strcmp(want, "wayland") == 0;
    if (want[0] && !want_x11 && !want_wl) {
        tw_fail(TW_ERR_ARG, "AETHER_TEST_WINDOW_SYSTEM=%s is neither x11 nor wayland", want);
        return TW_KIND_NONE;
    }
#if defined(TW_HAVE_X11)
    if (!want_wl && tw_x11_available()) return TW_KIND_X11;
    if (want_x11) return TW_KIND_NONE;
#else
    if (want_x11) {
        tw_fail(TW_ERR_UNAVAILABLE, "built without the X11 headers (install libx11-dev and rebuild)");
        return TW_KIND_NONE;
    }
#endif
#if defined(TW_HAVE_WAYLAND)
    if (tw_wl_available()) return TW_KIND_WAYLAND;
#else
    if (want_wl) tw_fail(TW_ERR_UNAVAILABLE, "built without the Wayland headers (install libwayland-dev and rebuild)");
#endif
    return TW_KIND_NONE;
}

int tw_available(void) { return tw_pick_system() != TW_KIND_NONE; }

TwWindow* tw_create(const char* title, int width, int height) {
    tw_clear_error();
    if (width <= 0 || height <= 0) {
        tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
        return NULL;
    }
    switch (tw_pick_system()) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return tw_x11_create(title, width, height);
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: return tw_wl_create(title, width, height);
#endif
        default:              return NULL;
    }
}

void tw_destroy(TwWindow* w) {
    if (!w) return;
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     tw_x11_destroy(w); return;
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: tw_wl_destroy(w); return;
#endif
        default:              free(w); return;
    }
}

int tw_pump(TwWindow* w) {
    if (!w) return 0;
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return tw_x11_pump(w);
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: return tw_wl_pump(w);
#endif
        default:              return 0;
    }
}

int tw_set_size(TwWindow* w, int width, int height) {
    tw_clear_error();
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    if (width <= 0 || height <= 0) {
        return tw_fail(TW_ERR_ARG, "window size must be positive, got %dx%d", width, height);
    }
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return tw_x11_set_size(w, width, height);
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: return tw_wl_set_size(w, width, height);
#endif
        default:              return tw_fail(TW_ERR_ARG, "not a window");
    }
}

int tw_set_minimized(TwWindow* w, int on) {
    (void)on;
    if (!w) return tw_fail(TW_ERR_ARG, "window is null");
    if (w->kind == TW_KIND_WAYLAND) {
        return tw_fail(TW_ERR_UNSUPPORTED, "xdg-shell has no request to restore a minimised window");
    }
    return tw_fail(TW_ERR_UNSUPPORTED, "an iconified X11 window keeps its size");
}

int   tw_kind(const TwWindow* w)    { return w ? w->kind : TW_KIND_NONE; }

void* tw_handle(const TwWindow* w) {
    if (!w) return NULL;
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return (void*)(uintptr_t)w->x.win;
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: return (void*)w->w.surface;
#endif
        default:              return NULL;
    }
}

void* tw_display(const TwWindow* w) {
    if (!w) return NULL;
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return (void*)w->x.dpy;
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND: return (void*)w->w.display;
#endif
        default:              return NULL;
    }
}

int tw_pixel(TwWindow* w, int x, int y) {
    tw_clear_error();
    if (!w) { tw_fail(TW_ERR_ARG, "window is null"); return -1; }
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) {
        tw_fail(TW_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, w->width, w->height);
        return -1;
    }
    switch (w->kind) {
#if defined(TW_HAVE_X11)
        case TW_KIND_X11:     return tw_x11_pixel(w, x, y);
#endif
#if defined(TW_HAVE_WAYLAND)
        case TW_KIND_WAYLAND:
            /* A Wayland client cannot read the screen through the core
             * protocol; weston's capture protocol is what reads it (#2389). */
            return tw_wl_pixel(w, x, y);
#endif
        default:
            tw_fail(TW_ERR_ARG, "not a window");
            return -1;
    }
}

#else  /* neither the X11 nor the Wayland headers at build time */

static int tw_no_system(void) {
    return tw_fail(TW_ERR_UNAVAILABLE,
                      "built without the X11 or Wayland headers (install libx11-dev or libwayland-dev and rebuild)");
}

int tw_available(void) { tw_no_system(); return 0; }
TwWindow* tw_create(const char* title, int width, int height) {
    (void)title; (void)width; (void)height;
    tw_no_system();
    return NULL;
}
void  tw_destroy(TwWindow* w) { (void)w; }
int   tw_pump(TwWindow* w) { (void)w; return 0; }
int   tw_set_size(TwWindow* w, int width, int height) {
    (void)w; (void)width; (void)height;
    return tw_no_system();
}
int   tw_set_minimized(TwWindow* w, int on) { (void)w; (void)on; return tw_no_system(); }
int   tw_kind(const TwWindow* w)    { (void)w; return TW_KIND_NONE; }
void* tw_handle(const TwWindow* w)  { (void)w; return NULL; }
void* tw_display(const TwWindow* w) { (void)w; return NULL; }
int   tw_pixel(TwWindow* w, int x, int y) {
    (void)w; (void)x; (void)y;
    tw_no_system();
    return -1;
}

#endif /* TW_HAVE_X11 || TW_HAVE_WAYLAND */
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
