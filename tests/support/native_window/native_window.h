/* Test fixture: a native top-level window for the GPU presentation tests.
 *
 * contrib.vulkan, contrib.d3d12 and contrib.metal present into a window they
 * are handed and never own one (#1505: windowing belongs to the toolkit, and
 * the GPU tier takes the handle as an opaque value from it). The toolkit is
 * aether-ui, whose native_view hands out exactly these handle kinds. But
 * aether-ui is built on this compiler, so this repository's CI cannot use it
 * to test presentation; this fixture stands in for it there. It is test
 * infrastructure, not an importable module: the tests declare the tw_ae_*
 * externs themselves and pass this file with --extra.
 *
 *   Windows  a Win32 window                    kind 1, handle = HWND
 *   macOS    an AppKit window's content view   kind 2, handle = NSView*
 *   Linux    an X11 window                     kind 3, handle = Window,
 *                                              display = Display*
 *
 * The kinds are numbered as aether-ui's native_view_kind() numbers them.
 * Nothing links against a window-system library: X11 and AppKit are opened at
 * runtime, so a test builds everywhere and skips where there is no display.
 * A window is used from one thread; on macOS that is the main thread.
 */

#ifndef AETHER_TEST_NATIVE_WINDOW_H
#define AETHER_TEST_NATIVE_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

#define TW_OK               0
#define TW_ERR_UNAVAILABLE -1   /* no window system reachable            */
#define TW_ERR_ARG         -2   /* caller passed something impossible    */
#define TW_ERR_OOM         -3   /* allocation failed                     */
#define TW_ERR_UNSUPPORTED -4   /* not possible on this window system    */
#define TW_ERR_SYSTEM      -5   /* the window system refused the request */

#define TW_KIND_NONE    0
#define TW_KIND_WIN32   1
#define TW_KIND_NSVIEW  2
#define TW_KIND_X11     3
#define TW_KIND_WAYLAND 4

typedef struct TwWindow TwWindow;

/* 1 when a window can be created here: always on Windows; on Linux when
 * libX11 loads and $DISPLAY opens; on macOS when AppKit loads. */
int tw_available(void);

/* Text for the most recent failure on the calling thread; "" when none. */
const char* tw_last_error(void);

/* A visible window whose client area is `width` x `height` PIXELS. Returns
 * once it is on screen. NULL on failure. */
TwWindow* tw_create(const char* title, int width, int height);
void      tw_destroy(TwWindow* w);

/* Handles pending events without blocking: 1 while open, 0 once the user
 * asked to close it. */
int tw_pump(TwWindow* w);

/* The client area in pixels, as last reported by the window system. */
int tw_width(const TwWindow* w);
int tw_height(const TwWindow* w);

/* 1 once after the client area changed size, then 0 until it changes again. */
int tw_take_resized(TwWindow* w);

/* Asks for a client area of `width` x `height` pixels and waits, up to a
 * second, for the window system to apply it; read tw_width / tw_height for
 * what was granted. */
int tw_set_size(TwWindow* w, int width, int height);

/* Minimises (1) or restores (0) the window. Only Win32 shrinks a minimised
 * window's surface to 0 x 0, which is the case this exists to test; the
 * other window systems refuse with TW_ERR_UNSUPPORTED. */
int tw_set_minimized(TwWindow* w, int on);

int   tw_kind(const TwWindow* w);
void* tw_handle(const TwWindow* w);
/* The X11 Display* for kind 3; NULL for the kinds that need no connection. */
void* tw_display(const TwWindow* w);

/* The colour on screen at client pixel (x, y), packed 0xRRGGBB, or -1 with
 * the reason in tw_last_error. Windows and X11; macOS refuses, because
 * reading another layer's pixels off the screen needs the screen-recording
 * permission a test runner does not have. */
int tw_pixel(TwWindow* w, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_TEST_NATIVE_WINDOW_H */
