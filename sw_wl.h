/* sw_wl.h — a Wayland layer-shell window with a software canvas in it.
 *
 * One surface on a layer (overlay, top, ...), a two-buffer shm pool, pointer
 * and keyboard from the seat, and a cursor asked for by name. The program
 * gets a Canvas to draw into and callbacks for input; nothing here knows what
 * is being drawn.
 *
 * The protocol glue for wlr-layer-shell-unstable-v1 and cursor-shape-v1 is
 * vendored below, the same hand-written marshalling swbr has shipped all
 * along, so there is no wayland-scanner step and no generated sources in the
 * tree. Only libwayland-client is linked.
 *
 * Keyboard text comes from the keymap the compositor sends, parsed here — see
 * sw_keymap.h. No libxkbcommon.
 *
 * Define SW_WL_IMPL in exactly one translation unit. sw_draw.h must be
 * included (with its implementation) by the same program.
 *
 * Written/modified by: Claude Opus 5
 */
#ifndef SW_WL_H
#define SW_WL_H

#include <stdbool.h>
#include <stdint.h>
#include "sw_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SwWin SwWin;

typedef enum {
    SW_LAYER_BACKGROUND = 0, SW_LAYER_BOTTOM = 1,
    SW_LAYER_TOP = 2, SW_LAYER_OVERLAY = 3
} SwLayer;

typedef enum { SW_KBD_NONE = 0, SW_KBD_EXCLUSIVE = 1, SW_KBD_ON_DEMAND = 2 } SwKbd;

enum { SW_ANCHOR_TOP = 1, SW_ANCHOR_BOTTOM = 2, SW_ANCHOR_LEFT = 4, SW_ANCHOR_RIGHT = 8 };

/* the cursor-shape-v1 shapes worth naming */
enum { SW_CURSOR_DEFAULT = 1, SW_CURSOR_POINTER = 10, SW_CURSOR_TEXT = 6,
       SW_CURSOR_GRAB = 11, SW_CURSOR_GRABBING = 12 };

enum { SW_MOD_SHIFT = 1, SW_MOD_CTRL = 2, SW_MOD_ALT = 4, SW_MOD_LOGO = 8 };

typedef struct {
    int         w, h;            /* 0 on an axis anchored both ways = fill it */
    int         anchor;          /* SW_ANCHOR_* bits; 0 centres the surface   */
    SwLayer     layer;
    SwKbd       keyboard;
    int         exclusive_zone;  /* -1 = sit over everything, do not reserve  */
    int         margin_top, margin_right, margin_bottom, margin_left;
    const char *namespace_;      /* what the compositor calls it              */
    const char *output;          /* connector name, NULL = compositor picks   */
} SwWinOpts;

/* Input as a queue, for a program with its own loop. The callbacks below
 * still fire; this is the same events, kept so an immediate-mode draw loop
 * can read them where it already reads everything else. Button numbers are
 * the Linux input codes Wayland uses. */
enum { SW_BTN_LEFT = 0x110, SW_BTN_RIGHT = 0x111, SW_BTN_MIDDLE = 0x112 };

typedef enum {
    SW_EV_NONE = 0, SW_EV_MOTION, SW_EV_BUTTON, SW_EV_AXIS, SW_EV_KEY,
    SW_EV_LEAVE, SW_EV_KB_ENTER, SW_EV_KB_LEAVE, SW_EV_CONFIGURE, SW_EV_CLOSED
} SwEvType;

typedef struct {
    SwEvType type;
    float    x, y;            /* motion, button */
    int      button;
    bool     pressed;         /* button, key */
    float    ax, ay;          /* axis, in surface units */
    uint32_t keysym;          /* key */
    uint32_t code;            /* key: the raw evdev keycode, for the key's
                                 place on the board rather than what it types */
    unsigned mods;
    char     text[8];         /* what the key typed, "" for none */
    int      w, h;            /* configure */
} SwEvent;

bool sw_poll(SwEvent *out);   /* false when the queue is empty */

typedef struct {
    void *user;
    /* Draw a frame. The canvas is already cleared to nothing. */
    void (*paint)(void *user, Canvas *cv, float scale);
    void (*pointer_motion)(void *user, float x, float y);
    void (*pointer_button)(void *user, int button, bool pressed, float x, float y);
    void (*pointer_axis)(void *user, float dx, float dy);
    void (*pointer_leave)(void *user);
    /* keysym is an X keysym; utf8 is what the key types, "" when it types
       nothing. mods is SW_MOD_* bits. */
    void (*key)(void *user, uint32_t keysym, const char *utf8, unsigned mods,
                bool pressed);
    void (*keyboard_leave)(void *user);
    void (*configured)(void *user, int w, int h, float scale);
    void (*closed)(void *user);
} SwApp;

bool    sw_init(void);                 /* connect and bind; false if no display */
void    sw_shutdown(void);
const char *sw_error(void);

SwWin  *sw_win_create(const SwWinOpts *opts, const SwApp *app);
void    sw_win_destroy(SwWin *w);
void    sw_win_damage(SwWin *w);       /* ask for a repaint */
void    sw_win_resize(SwWin *w, int width, int height);
void    sw_win_keyboard(SwWin *w, SwKbd k);
void    sw_win_cursor(SwWin *w, unsigned shape);
int     sw_win_width(const SwWin *w);
int     sw_win_height(const SwWin *w);
float   sw_win_scale(const SwWin *w);

/* For a program that drives its own frames — an animation loop rather than
 * a repaint on change. Begin hands back the canvas for the next buffer, or
 * NULL when the surface is not ready or both buffers are still on screen;
 * end attaches and commits it. Between the two, draw. A program that uses
 * these leaves app.paint NULL. */
Canvas *sw_win_begin(SwWin *w);
void    sw_win_end(SwWin *w);

int     sw_fd(void);                   /* the display fd, for your own poll() */
bool    sw_flush(void);
bool    sw_dispatch_pending(void);     /* handle what has arrived */
bool    sw_dispatch(int timeout_ms);   /* poll, read, dispatch, repaint */
void    sw_run(volatile bool *running);

#ifdef __cplusplus
}
#endif
#endif /* SW_WL_H */

/* ===================================================================== */
#ifdef SW_WL_IMPL
#ifndef SW_WL_IMPL_ONCE
#define SW_WL_IMPL_ONCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "sw_keymap.h"

/* ------------------------------------------- cursor-shape-v1 glue */
/* Vendored equivalent of what wayland-scanner emits for
 * cursor-shape-v1.xml. Two interfaces and two requests: ask the compositor
 * for a named cursor over our surface, rather than shipping a cursor theme
 * and drawing one ourselves. Absent on an older compositor, in which case
 * nothing here runs and the pointer keeps whatever shape it had. */

struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;

extern const struct wl_interface wp_cursor_shape_device_v1_interface;

static const struct wl_interface *sw_cs_types[] = {
    NULL, NULL,
    &wp_cursor_shape_device_v1_interface,
    &wl_pointer_interface,
    NULL, NULL,
};

static const struct wl_message wp_cursor_shape_manager_v1_requests[] = {
    { "destroy", "", sw_cs_types + 0 },
    { "get_pointer", "no", sw_cs_types + 2 },
    { "get_tablet_tool_v2", "no", sw_cs_types + 4 },
};
static const struct wl_interface wp_cursor_shape_manager_v1_interface = {
    "wp_cursor_shape_manager_v1", 1, 3, wp_cursor_shape_manager_v1_requests, 0, NULL,
};

static const struct wl_message wp_cursor_shape_device_v1_requests[] = {
    { "destroy", "", sw_cs_types + 0 },
    { "set_shape", "uu", sw_cs_types + 0 },
};
const struct wl_interface wp_cursor_shape_device_v1_interface = {
    "wp_cursor_shape_device_v1", 1, 2, wp_cursor_shape_device_v1_requests, 0, NULL,
};

enum wp_cursor_shape_device_v1_shape {
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT = 1,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER = 4,
};

static struct wp_cursor_shape_device_v1 *
wp_cursor_shape_manager_v1_get_pointer(struct wp_cursor_shape_manager_v1 *m,
                                       struct wl_pointer *p)
{
    struct wl_proxy *id = wl_proxy_marshal_flags(
        (struct wl_proxy *)m, 1, &wp_cursor_shape_device_v1_interface,
        wl_proxy_get_version((struct wl_proxy *)m), 0, NULL, p);
    return (struct wp_cursor_shape_device_v1 *)id;
}

static void wp_cursor_shape_device_v1_set_shape(struct wp_cursor_shape_device_v1 *d,
                                                uint32_t serial, uint32_t shape)
{
    wl_proxy_marshal_flags((struct wl_proxy *)d, 1, NULL,
                           wl_proxy_get_version((struct wl_proxy *)d), 0,
                           serial, shape);
}

/* ------------------------------------------------- wlr-layer-shell glue */
/* Vendored equivalent of what wayland-scanner emits for
 * wlr-layer-shell-unstable-v1.xml (Copyright (c) 2017 Drew DeVault, MIT-ish;
 * see the protocol file for the full notice). Only the requests and events
 * we actually use are wrapped; get_popup is present in the table so the
 * opcodes line up, but is never called, hence its NULL type entry. */

enum zwlr_layer_shell_v1_layer {
    ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND = 0,
    ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM     = 1,
    ZWLR_LAYER_SHELL_V1_LAYER_TOP        = 2,
    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY    = 3
};
enum zwlr_layer_surface_v1_anchor {
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    = 1,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM = 2,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   = 4,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT  = 8
};
enum zwlr_layer_surface_v1_keyboard_interactivity {
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE      = 0,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE = 1,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND = 2
};

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

extern const struct wl_interface zwlr_layer_surface_v1_interface;

static const struct wl_interface *sw_ls_types[] = {
    NULL, NULL, NULL, NULL,
    &zwlr_layer_surface_v1_interface,
    &wl_surface_interface,
    &wl_output_interface,
    NULL, NULL, NULL,
};

static const struct wl_message zwlr_layer_shell_v1_requests[] = {
    { "get_layer_surface", "no?ous", sw_ls_types + 4 },
    { "destroy", "3", sw_ls_types + 0 },
};
static const struct wl_interface zwlr_layer_shell_v1_interface = {
    "zwlr_layer_shell_v1", 4, 2, zwlr_layer_shell_v1_requests, 0, NULL,
};

static const struct wl_message zwlr_layer_surface_v1_requests[] = {
    { "set_size", "uu", sw_ls_types + 0 },
    { "set_anchor", "u", sw_ls_types + 0 },
    { "set_exclusive_zone", "i", sw_ls_types + 0 },
    { "set_margin", "iiii", sw_ls_types + 0 },
    { "set_keyboard_interactivity", "u", sw_ls_types + 0 },
    { "get_popup", "o", sw_ls_types + 9 },
    { "ack_configure", "u", sw_ls_types + 0 },
    { "destroy", "", sw_ls_types + 0 },
    { "set_layer", "2u", sw_ls_types + 0 },
};
static const struct wl_message zwlr_layer_surface_v1_events[] = {
    { "configure", "uuu", sw_ls_types + 0 },
    { "closed", "", sw_ls_types + 0 },
};
const struct wl_interface zwlr_layer_surface_v1_interface = {
    "zwlr_layer_surface_v1", 4, 9, zwlr_layer_surface_v1_requests,
    2, zwlr_layer_surface_v1_events,
};

struct zwlr_layer_surface_v1_listener {
    void (*configure)(void *data, struct zwlr_layer_surface_v1 *s,
                      uint32_t serial, uint32_t width, uint32_t height);
    void (*closed)(void *data, struct zwlr_layer_surface_v1 *s);
};

#define LS_REQ_SET_SIZE        0
#define LS_REQ_SET_ANCHOR      1
#define LS_REQ_SET_EXCLUSIVE   2
#define LS_REQ_SET_MARGIN      3
#define LS_REQ_SET_KBD         4
#define LS_REQ_ACK_CONFIGURE   6
#define LS_REQ_DESTROY         7

static struct zwlr_layer_surface_v1 *
zwlr_layer_shell_v1_get_layer_surface(struct zwlr_layer_shell_v1 *shell,
                                      struct wl_surface *surface,
                                      struct wl_output *output,
                                      uint32_t layer, const char *ns)
{
    struct wl_proxy *id = wl_proxy_marshal_flags(
        (struct wl_proxy *)shell, 0, &zwlr_layer_surface_v1_interface,
        wl_proxy_get_version((struct wl_proxy *)shell), 0,
        NULL, surface, output, layer, ns);
    return (struct zwlr_layer_surface_v1 *)id;
}

static int zwlr_layer_surface_v1_add_listener(struct zwlr_layer_surface_v1 *s,
                                              const struct zwlr_layer_surface_v1_listener *l,
                                              void *data)
{
    return wl_proxy_add_listener((struct wl_proxy *)s, (void (**)(void))l, data);
}

static void ls_set_size(struct zwlr_layer_surface_v1 *s, uint32_t w, uint32_t h)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_SIZE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, w, h); }
static void ls_set_anchor(struct zwlr_layer_surface_v1 *s, uint32_t a)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_ANCHOR, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, a); }
static void ls_set_exclusive_zone(struct zwlr_layer_surface_v1 *s, int32_t z)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_EXCLUSIVE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, z); }
static void ls_set_margin(struct zwlr_layer_surface_v1 *s, int32_t t, int32_t r,
                          int32_t b, int32_t l)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_MARGIN, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, t, r, b, l); }
static void ls_set_keyboard(struct zwlr_layer_surface_v1 *s, uint32_t k)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_KBD, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, k); }
static void ls_ack_configure(struct zwlr_layer_surface_v1 *s, uint32_t serial)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_ACK_CONFIGURE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, serial); }
static void ls_destroy(struct zwlr_layer_surface_v1 *s)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_DESTROY, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s),
                         WL_MARSHAL_FLAG_DESTROY); }


/* --------------------------------------------------------------- state */

/* Under -std=c11 the libc headers hide these, and a feature macro set in a
 * header that is included after the program's own would come too late to do
 * anything. They are POSIX and Linux; these are their real declarations. */
#if defined(__STRICT_ANSI__)
int memfd_create(const char *name, unsigned int flags);
int ftruncate(int fd, off_t length);
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

static struct wl_display    *SWD;
static struct wl_registry   *SWREG;
static struct wl_compositor *SWCOMP;
static struct wl_shm        *SWSHM;
static struct wl_seat       *SWSEAT;
static struct zwlr_layer_shell_v1 *SWSHELL;
static struct wp_cursor_shape_manager_v1 *SWCSM;
static struct wl_pointer    *SWPTR;
static struct wl_keyboard   *SWKBD;
static struct wp_cursor_shape_device_v1 *SWCSD;
static uint32_t              SWPTR_SERIAL;
static char                  SWERR[256];

/* Outputs, only so a window can ask for one by connector name. */
typedef struct {
    struct wl_output *out;
    uint32_t name;
    char     conn[64];
    int32_t  scale;
} SwOutput;
static SwOutput SWOUT[8];
static int      SWOUT_N;

#define SW_MAX_WIN 4
static SwWin   *SWWINS[SW_MAX_WIN];
static int      SWWIN_N;
static SwWin   *SW_PTR_WIN;     /* the one the pointer is over */
static SwWin   *SW_KBD_WIN;     /* the one with keyboard focus */

struct SwWin {
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *ls;
    SwApp  app;
    SwWinOpts opts;

    int    w, h;                 /* logical */
    int    scale;
    bool   configured, dirty, closed;

    struct wl_shm_pool *pool;
    void  *pool_data;
    size_t pool_size;
    struct wl_buffer *buf[2];
    void  *bufmem[2];
    bool   busy[2];
    int    bw, bh, stride;

    unsigned cursor;
    int    slot;                 /* the buffer sw_win_begin handed out */
};

const char *sw_error(void) { return SWERR; }

/* A ring, deliberately small: these are human-speed events and a frame drains
 * it. Dropping the oldest on overflow is better than growing without bound. */
#define SW_EVQ 128
static SwEvent SW_Q[SW_EVQ];
static int     SW_QH, SW_QT;

static void sw_push(const SwEvent *e)
{
    int next = (SW_QT + 1) % SW_EVQ;
    if (next == SW_QH) SW_QH = (SW_QH + 1) % SW_EVQ;   /* drop the oldest */
    SW_Q[SW_QT] = *e;
    SW_QT = next;
}

bool sw_poll(SwEvent *out)
{
    if (SW_QH == SW_QT) return false;
    *out = SW_Q[SW_QH];
    SW_QH = (SW_QH + 1) % SW_EVQ;
    return true;
}

/* -------------------------------------------------------------- buffers */

static void sw_buffer_release(void *data, struct wl_buffer *b)
{
    SwWin *w = (SwWin *)data;
    for (int i = 0; i < 2; ++i) if (w->buf[i] == b) w->busy[i] = false;
}
static const struct wl_buffer_listener sw_buffer_listener = { sw_buffer_release };

static void sw_free_buffers(SwWin *w)
{
    for (int i = 0; i < 2; ++i) {
        if (w->buf[i]) wl_buffer_destroy(w->buf[i]);
        w->buf[i] = NULL;
        w->busy[i] = false;
        w->bufmem[i] = NULL;
    }
    if (w->pool) wl_shm_pool_destroy(w->pool);
    w->pool = NULL;
    if (w->pool_data) munmap(w->pool_data, w->pool_size);
    w->pool_data = NULL;
    w->pool_size = 0;
}

static int sw_anon_shm(size_t size)
{
    int fd = memfd_create("sw_wl", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

/* Two buffers in one pool: one on screen while the next is drawn. */
static bool sw_alloc(SwWin *w)
{
    int bw = w->w * w->scale, bh = w->h * w->scale;
    if (bw <= 0 || bh <= 0) return false;
    if (w->pool && w->bw == bw && w->bh == bh) return true;

    sw_free_buffers(w);
    w->bw = bw;
    w->bh = bh;
    w->stride = bw * 4;
    size_t one = (size_t)w->stride * (size_t)bh;
    size_t total = one * 2;

    int fd = sw_anon_shm(total);
    if (fd < 0) return false;
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return false; }

    w->pool_data = mem;
    w->pool_size = total;
    w->pool = wl_shm_create_pool(SWSHM, fd, (int32_t)total);
    close(fd);
    for (int i = 0; i < 2; ++i) {
        w->buf[i] = wl_shm_pool_create_buffer(w->pool, (int32_t)(one * (size_t)i),
                                              bw, bh, w->stride,
                                              WL_SHM_FORMAT_ARGB8888);
        w->bufmem[i] = (char *)mem + one * (size_t)i;
        w->busy[i] = false;
        wl_buffer_add_listener(w->buf[i], &sw_buffer_listener, w);
    }
    return true;
}

/* ---------------------------------------------------------------- paint */

static void sw_repaint(SwWin *w)
{
    if (!w->configured || w->closed || !w->app.paint) return;
    if (!sw_alloc(w)) return;

    int slot = -1;
    for (int i = 0; i < 2; ++i) if (!w->busy[i]) { slot = i; break; }
    if (slot < 0) return;                     /* both in flight; try again */

    Canvas cv = { (uint32_t *)w->bufmem[slot], w->bw, w->bh,
                  0, 0, w->bw, w->bh };
    memset(cv.px, 0, (size_t)w->stride * (size_t)w->bh);
    w->app.paint(w->app.user, &cv, (float)w->scale);

    wl_surface_set_buffer_scale(w->surf, w->scale);
    wl_surface_attach(w->surf, w->buf[slot], 0, 0);
    wl_surface_damage_buffer(w->surf, 0, 0, w->bw, w->bh);
    wl_surface_commit(w->surf);
    w->busy[slot] = true;
    w->dirty = false;
}

Canvas *sw_win_begin(SwWin *w)
{
    static Canvas cv;
    if (!w || !w->configured || w->closed) return NULL;
    if (!sw_alloc(w)) return NULL;
    int slot = -1;
    for (int i = 0; i < 2; ++i) if (!w->busy[i]) { slot = i; break; }
    if (slot < 0) return NULL;                /* both in flight; skip a frame */
    w->slot = slot;
    cv.px = (uint32_t *)w->bufmem[slot];
    cv.w = w->bw;
    cv.h = w->bh;
    cv.cx0 = 0; cv.cy0 = 0; cv.cx1 = w->bw; cv.cy1 = w->bh;
    memset(cv.px, 0, (size_t)w->stride * (size_t)w->bh);
    return &cv;
}

void sw_win_end(SwWin *w)
{
    if (!w || w->slot < 0) return;
    wl_surface_set_buffer_scale(w->surf, w->scale);
    wl_surface_attach(w->surf, w->buf[w->slot], 0, 0);
    wl_surface_damage_buffer(w->surf, 0, 0, w->bw, w->bh);
    wl_surface_commit(w->surf);
    w->busy[w->slot] = true;
    w->slot = -1;
    w->dirty = false;
}

void sw_win_damage(SwWin *w) { if (w) w->dirty = true; }
int   sw_win_width(const SwWin *w)  { return w ? w->w : 0; }
int   sw_win_height(const SwWin *w) { return w ? w->h : 0; }
float sw_win_scale(const SwWin *w)  { return w ? (float)w->scale : 1.0f; }

/* -------------------------------------------------------- layer surface */

static void sw_ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    SwWin *w = (SwWin *)data;
    ls_ack_configure(ls, serial);
    bool changed = (width && (int)width != w->w) ||
                   (height && (int)height != w->h) || !w->configured;
    if (width)  w->w = (int)width;
    if (height) w->h = (int)height;
    w->configured = true;
    if (!changed) return;
    SwEvent e = { .type = SW_EV_CONFIGURE, .w = w->w, .h = w->h };
    sw_push(&e);
    if (w->app.configured) w->app.configured(w->app.user, w->w, w->h, (float)w->scale);
    w->dirty = true;
    sw_repaint(w);
}

static void sw_ls_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    (void)ls;
    SwWin *w = (SwWin *)data;
    w->closed = true;
    SwEvent e = { .type = SW_EV_CLOSED };
    sw_push(&e);
    if (w->app.closed) w->app.closed(w->app.user);
}

static const struct zwlr_layer_surface_v1_listener sw_ls_listener = {
    sw_ls_configure, sw_ls_closed
};

/* The output's scale reaches us after the surface exists, so a window on a
 * HiDPI screen is created at 1x and re-allocated when the real figure lands. */
static void sw_surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
    (void)s;
    SwWin *w = (SwWin *)data;
    for (int i = 0; i < SWOUT_N; ++i)
        if (SWOUT[i].out == o && SWOUT[i].scale > 0 && SWOUT[i].scale != w->scale) {
            w->scale = SWOUT[i].scale;
            w->dirty = true;
        }
}
static void sw_surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{ (void)data; (void)s; (void)o; }
static const struct wl_surface_listener sw_surf_listener = {
    sw_surf_enter, sw_surf_leave, NULL, NULL
};

SwWin *sw_win_create(const SwWinOpts *opts, const SwApp *app)
{
    if (!SWCOMP || !SWSHELL || !SWSHM) {
        snprintf(SWERR, sizeof(SWERR), "compositor is missing layer-shell or shm");
        return NULL;
    }
    if (SWWIN_N >= SW_MAX_WIN) return NULL;

    SwWin *w = (SwWin *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->opts = *opts;
    if (app) w->app = *app;      /* NULL: the program drives its own frames */
    w->scale = 1;
    w->slot = -1;
    w->w = opts->w;
    w->h = opts->h;
    w->cursor = SW_CURSOR_DEFAULT;

    struct wl_output *on = NULL;
    if (opts->output && *opts->output)
        for (int i = 0; i < SWOUT_N; ++i)
            if (!strcmp(SWOUT[i].conn, opts->output)) { on = SWOUT[i].out; break; }

    w->surf = wl_compositor_create_surface(SWCOMP);
    wl_surface_add_listener(w->surf, &sw_surf_listener, w);
    w->ls = zwlr_layer_shell_v1_get_layer_surface(
                SWSHELL, w->surf, on, (uint32_t)opts->layer,
                opts->namespace_ ? opts->namespace_ : "sw");
    zwlr_layer_surface_v1_add_listener(w->ls, &sw_ls_listener, w);
    ls_set_size(w->ls, (uint32_t)(opts->w > 0 ? opts->w : 0),
                                   (uint32_t)(opts->h > 0 ? opts->h : 0));
    ls_set_anchor(w->ls, (uint32_t)opts->anchor);
    ls_set_exclusive_zone(w->ls, opts->exclusive_zone);
    ls_set_margin(w->ls, opts->margin_top, opts->margin_right,
                                     opts->margin_bottom, opts->margin_left);
    ls_set_keyboard(w->ls, (uint32_t)opts->keyboard);
    wl_surface_commit(w->surf);

    SWWINS[SWWIN_N++] = w;
    return w;
}

void sw_win_destroy(SwWin *w)
{
    if (!w) return;
    for (int i = 0; i < SWWIN_N; ++i)
        if (SWWINS[i] == w) { SWWINS[i] = SWWINS[--SWWIN_N]; break; }
    if (SW_PTR_WIN == w) SW_PTR_WIN = NULL;
    if (SW_KBD_WIN == w) SW_KBD_WIN = NULL;
    sw_free_buffers(w);
    if (w->ls) ls_destroy(w->ls);
    if (w->surf) wl_surface_destroy(w->surf);
    free(w);
}

void sw_win_resize(SwWin *w, int width, int height)
{
    if (!w || !w->ls) return;
    ls_set_size(w->ls, (uint32_t)(width > 0 ? width : 0),
                                   (uint32_t)(height > 0 ? height : 0));
    wl_surface_commit(w->surf);
}

void sw_win_keyboard(SwWin *w, SwKbd k)
{
    if (!w || !w->ls) return;
    ls_set_keyboard(w->ls, (uint32_t)k);
    wl_surface_commit(w->surf);
}

void sw_win_cursor(SwWin *w, unsigned shape)
{
    if (!w) return;
    w->cursor = shape;
    if (SWCSD && SW_PTR_WIN == w && SWPTR_SERIAL)
        wp_cursor_shape_device_v1_set_shape(SWCSD, SWPTR_SERIAL, shape);
}

/* --------------------------------------------------------------- input */

static SwWin *sw_win_of(struct wl_surface *s)
{
    for (int i = 0; i < SWWIN_N; ++i) if (SWWINS[i]->surf == s) return SWWINS[i];
    return NULL;
}

static double SW_PX, SW_PY;



static void sw_ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                         struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p;
    SwWin *w = sw_win_of(s);
    if (!w) return;
    SW_PTR_WIN = w;
    SWPTR_SERIAL = serial;
    SW_PX = wl_fixed_to_double(x);
    SW_PY = wl_fixed_to_double(y);
    if (SWCSD) wp_cursor_shape_device_v1_set_shape(SWCSD, serial, w->cursor);
    SwEvent e = { .type = SW_EV_MOTION, .x = (float)SW_PX, .y = (float)SW_PY };
    sw_push(&e);
    if (w->app.pointer_motion) w->app.pointer_motion(w->app.user, (float)SW_PX, (float)SW_PY);
}

static void sw_ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                         struct wl_surface *s)
{
    (void)d; (void)p; (void)serial;
    SwWin *w = sw_win_of(s);
    if (!w) return;
    /* A leave for a surface we already left, arriving after the enter for
       another, must not clear the pointer from the one we are on. */
    if (SW_PTR_WIN == w) SW_PTR_WIN = NULL;
    SwEvent e = { .type = SW_EV_LEAVE };
    sw_push(&e);
    if (w->app.pointer_leave) w->app.pointer_leave(w->app.user);
}

static void sw_ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    SW_PX = wl_fixed_to_double(x);
    SW_PY = wl_fixed_to_double(y);
    SwWin *w = SW_PTR_WIN;
    SwEvent e = { .type = SW_EV_MOTION, .x = (float)SW_PX, .y = (float)SW_PY };
    sw_push(&e);
    if (w && w->app.pointer_motion)
        w->app.pointer_motion(w->app.user, (float)SW_PX, (float)SW_PY);
}

static void sw_ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                          uint32_t t, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)t;
    SWPTR_SERIAL = serial;
    SwWin *w = SW_PTR_WIN;
    SwEvent e = { .type = SW_EV_BUTTON, .x = (float)SW_PX, .y = (float)SW_PY,
                  .button = (int)button, .pressed = state != 0 };
    sw_push(&e);
    if (w && w->app.pointer_button)
        w->app.pointer_button(w->app.user, (int)button, state != 0,
                              (float)SW_PX, (float)SW_PY);
}

static void sw_ptr_axis(void *d, struct wl_pointer *p, uint32_t t,
                        uint32_t axis, wl_fixed_t value)
{
    (void)d; (void)p; (void)t;
    SwWin *w = SW_PTR_WIN;
    double v = wl_fixed_to_double(value);
    SwEvent e = { .type = SW_EV_AXIS };
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) e.ay = (float)v; else e.ax = (float)v;
    sw_push(&e);
    if (!w || !w->app.pointer_axis) return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) w->app.pointer_axis(w->app.user, 0.0f, (float)v);
    else                                         w->app.pointer_axis(w->app.user, (float)v, 0.0f);
}

static void sw_ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void sw_ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void sw_ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void sw_ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void sw_ptr_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void sw_ptr_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir)
{ (void)d; (void)p; (void)a; (void)dir; }

static const struct wl_pointer_listener sw_ptr_listener = {
    sw_ptr_enter, sw_ptr_leave, sw_ptr_motion, sw_ptr_button, sw_ptr_axis,
    sw_ptr_frame, sw_ptr_axis_src, sw_ptr_axis_stop, sw_ptr_axis_disc,
    sw_ptr_axis_v120, sw_ptr_axis_rel
};

/* ------------------------------------------------------------ keyboard */

static SwKeymap SW_KEYMAP;
static unsigned SW_MODS;

static void sw_kbd_keymap(void *d, struct wl_keyboard *k, uint32_t format,
                          int32_t fd, uint32_t size)
{
    (void)d; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *text = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (text != MAP_FAILED) {
        sw_keymap_parse(&SW_KEYMAP, text, size);
        munmap(text, size);
    }
    close(fd);
}

static void sw_kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                         struct wl_surface *s, struct wl_array *keys)
{
    (void)d; (void)k; (void)serial; (void)keys;
    SW_KBD_WIN = sw_win_of(s);
    SwEvent e = { .type = SW_EV_KB_ENTER };
    sw_push(&e);
}

static void sw_kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                         struct wl_surface *s)
{
    (void)d; (void)k; (void)serial;
    SwWin *w = sw_win_of(s);
    SwEvent e = { .type = SW_EV_KB_LEAVE };
    sw_push(&e);
    if (w && w->app.keyboard_leave) w->app.keyboard_leave(w->app.user);
    if (SW_KBD_WIN == w) SW_KBD_WIN = NULL;
}

static void sw_kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                       uint32_t time, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)time;
    SwWin *w = SW_KBD_WIN;
    char utf8[8] = "";
    uint32_t sym = sw_keymap_lookup(&SW_KEYMAP, key + 8, SW_MODS, utf8, sizeof(utf8));
    SwEvent e = { .type = SW_EV_KEY, .keysym = sym, .code = key, .mods = SW_MODS,
                  .pressed = state != 0 };
    memcpy(e.text, utf8, sizeof(e.text));
    sw_push(&e);
    if (!w || !w->app.key) return;
    w->app.key(w->app.user, sym, utf8, SW_MODS, state != 0);
}

static void sw_kbd_mods(void *d, struct wl_keyboard *k, uint32_t serial,
                        uint32_t dep, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)d; (void)k; (void)serial; (void)latched; (void)group;
    SW_MODS = sw_keymap_mods(&SW_KEYMAP, dep | locked);
}

static void sw_kbd_rep(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d; (void)k; (void)rate; (void)delay; }

static const struct wl_keyboard_listener sw_kbd_listener = {
    sw_kbd_keymap, sw_kbd_enter, sw_kbd_leave, sw_kbd_key, sw_kbd_mods, sw_kbd_rep
};

static void sw_seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !SWPTR) {
        SWPTR = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(SWPTR, &sw_ptr_listener, NULL);
        if (SWCSM) SWCSD = wp_cursor_shape_manager_v1_get_pointer(SWCSM, SWPTR);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !SWKBD) {
        SWKBD = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(SWKBD, &sw_kbd_listener, NULL);
    }
}
static void sw_seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }
static const struct wl_seat_listener sw_seat_listener = { sw_seat_caps, sw_seat_name };

/* -------------------------------------------------------------- outputs */

static void sw_out_geom(void *d, struct wl_output *o, int32_t x, int32_t y,
                        int32_t pw, int32_t ph, int32_t sub, const char *make,
                        const char *model, int32_t tr)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
  (void)make; (void)model; (void)tr; }
static void sw_out_mode(void *d, struct wl_output *o, uint32_t f,
                        int32_t w, int32_t h, int32_t r)
{ (void)d; (void)o; (void)f; (void)w; (void)h; (void)r; }
static void sw_out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void sw_out_scale(void *d, struct wl_output *o, int32_t s)
{
    (void)d;
    for (int i = 0; i < SWOUT_N; ++i) if (SWOUT[i].out == o) SWOUT[i].scale = s;
}
static void sw_out_name(void *d, struct wl_output *o, const char *name)
{
    (void)d;
    for (int i = 0; i < SWOUT_N; ++i)
        if (SWOUT[i].out == o) snprintf(SWOUT[i].conn, sizeof(SWOUT[i].conn), "%s", name);
}
static void sw_out_desc(void *d, struct wl_output *o, const char *desc)
{ (void)d; (void)o; (void)desc; }
static const struct wl_output_listener sw_out_listener = {
    sw_out_geom, sw_out_mode, sw_out_done, sw_out_scale, sw_out_name, sw_out_desc
};

/* ------------------------------------------------------------- registry */

static void sw_reg_global(void *d, struct wl_registry *r, uint32_t name,
                          const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, "wl_compositor"))
        SWCOMP = (struct wl_compositor *)wl_registry_bind(r, name, &wl_compositor_interface,
                                                          ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_shm"))
        SWSHM = (struct wl_shm *)wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "zwlr_layer_shell_v1"))
        SWSHELL = (struct zwlr_layer_shell_v1 *)wl_registry_bind(
                      r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wp_cursor_shape_manager_v1"))
        SWCSM = (struct wp_cursor_shape_manager_v1 *)wl_registry_bind(
                    r, name, &wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(iface, "wl_seat")) {
        SWSEAT = (struct wl_seat *)wl_registry_bind(r, name, &wl_seat_interface,
                                                    ver < 5 ? ver : 5);
        wl_seat_add_listener(SWSEAT, &sw_seat_listener, NULL);
    } else if (!strcmp(iface, "wl_output") && SWOUT_N < 8) {
        SwOutput *o = &SWOUT[SWOUT_N++];
        memset(o, 0, sizeof(*o));
        o->name = name;
        o->scale = 1;
        o->out = (struct wl_output *)wl_registry_bind(r, name, &wl_output_interface,
                                                      ver < 4 ? ver : 4);
        wl_output_add_listener(o->out, &sw_out_listener, NULL);
    }
}

static void sw_reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r;
    for (int i = 0; i < SWOUT_N; ++i)
        if (SWOUT[i].name == name) {
            wl_output_destroy(SWOUT[i].out);
            SWOUT[i] = SWOUT[--SWOUT_N];
            break;
        }
}
static const struct wl_registry_listener sw_reg_listener = { sw_reg_global, sw_reg_remove };

/* ------------------------------------------------------------ lifecycle */

bool sw_init(void)
{
    SWD = wl_display_connect(NULL);
    if (!SWD) {
        snprintf(SWERR, sizeof(SWERR),
                 "cannot reach a Wayland compositor (is WAYLAND_DISPLAY set?)");
        return false;
    }
    SWREG = wl_display_get_registry(SWD);
    wl_registry_add_listener(SWREG, &sw_reg_listener, NULL);
    wl_display_roundtrip(SWD);     /* globals */
    wl_display_roundtrip(SWD);     /* and what they send back: seat caps, names */

    if (!SWCOMP || !SWSHM) {
        snprintf(SWERR, sizeof(SWERR), "compositor has no wl_compositor or wl_shm");
        return false;
    }
    if (!SWSHELL) {
        snprintf(SWERR, sizeof(SWERR),
                 "compositor does not support wlr-layer-shell; sway and the "
                 "wlroots compositors do");
        return false;
    }
    return true;
}

void sw_shutdown(void)
{
    while (SWWIN_N) sw_win_destroy(SWWINS[0]);
    if (SWPTR) wl_pointer_destroy(SWPTR);
    if (SWKBD) wl_keyboard_destroy(SWKBD);
    if (SWD) wl_display_disconnect(SWD);
    SWD = NULL;
}

int  sw_fd(void) { return SWD ? wl_display_get_fd(SWD) : -1; }
bool sw_flush(void) { return SWD && wl_display_flush(SWD) != -1; }

static void sw_paint_dirty(void)
{
    for (int i = 0; i < SWWIN_N; ++i)
        if (SWWINS[i]->dirty) sw_repaint(SWWINS[i]);
}

bool sw_dispatch_pending(void)
{
    if (!SWD) return false;
    if (wl_display_dispatch_pending(SWD) < 0) return false;
    sw_paint_dirty();
    return wl_display_flush(SWD) != -1;
}

bool sw_dispatch(int timeout_ms)
{
    if (!SWD) return false;
    sw_paint_dirty();

    while (wl_display_prepare_read(SWD) != 0)
        if (wl_display_dispatch_pending(SWD) < 0) return false;
    if (wl_display_flush(SWD) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(SWD);
        return false;
    }

    struct pollfd pfd = { wl_display_get_fd(SWD), POLLIN, 0 };
    int n = poll(&pfd, 1, timeout_ms);
    if (n > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(SWD) < 0) return false;
    } else {
        wl_display_cancel_read(SWD);
        if (n < 0 && errno != EINTR) return false;
    }
    if (wl_display_dispatch_pending(SWD) < 0) return false;
    sw_paint_dirty();
    return true;
}

void sw_run(volatile bool *running)
{
    while (*running && sw_dispatch(-1)) { }
}

#endif /* SW_WL_IMPL_ONCE */
#endif /* SW_WL_IMPL */
