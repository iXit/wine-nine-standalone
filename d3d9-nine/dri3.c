/*
 * Wine DRI3 interface
 *
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015 Patrick Rudolph
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#include "config.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

#include "dri3.h"

#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include "winbase.h"

#ifdef D3D9NINE_DRI2
#include <sys/ioctl.h>

#define BOOL X_BOOL
#define BYTE X_BYTE
#define INT8 X_INT8
#define INT16 X_INT16
#define INT32 X_INT32
#define INT64 X_INT64
#include <X11/Xmd.h>
#undef BOOL
#undef BYTE
#undef INT8
#undef INT16
#undef INT32
#undef INT64
#undef LONG64

#include <X11/Xlibint.h>
#include <X11/extensions/dri2tokens.h>
#include <X11/extensions/dri2proto.h>
#include <X11/extensions/extutil.h>
#define GL_GLEXT_PROTOTYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_LEGACY 1

/* workaround for broken ABI on x86_64 due to windef.h */
#undef APIENTRY
#undef APIENTRYP
#include <GL/gl.h>

/* workaround gl header bug */
#define glBlendColor glBlendColorLEV
#define glBlendEquation glBlendEquationLEV
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm.h>

static EGLDisplay display = NULL;
static int display_ref = 0;

struct DRI2priv {
    Display *dpy;
    EGLDisplay display;
    EGLContext context;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_func;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_func;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_func;
};
#endif

struct PRESENTPriv {
    xcb_connection_t *xcb_connection;
    xcb_connection_t *xcb_connection_bis; /* to avoid libxcb thread bugs, use a different connection to present pixmaps */
    XID window;
    uint64_t last_msc;
    uint64_t last_target;
    uint32_t last_serial_given;
    xcb_special_event_t *special_event;
    PRESENTPixmapPriv *first_present_priv;
    int pixmap_present_pending;
    BOOL idle_notify_since_last_check;
    BOOL notify_with_serial_pending;
    CRITICAL_SECTION mutex_present; /* protect readind/writing present_priv things */
    CRITICAL_SECTION mutex_xcb_wait;
    BOOL xcb_wait;
};

struct PRESENTPixmapPriv {
    PRESENTpriv *present_priv;
    Pixmap pixmap;
    BOOL released;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int present_complete_pending;
    uint32_t serial;
#ifdef D3D9NINE_DRI2
    struct {
        BOOL is_dri2;
        struct DRI2priv *dri2_priv;
        GLuint fbo_read;
        GLuint fbo_write;
        GLuint texture_read;
        GLuint texture_write;
    } dri2_info;
#endif
    BOOL last_present_was_flip;
    PRESENTPixmapPriv *next;
};

BOOL DRI3CheckExtension(Display *dpy, int major, int minor)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_dri3_query_version_cookie_t dri3_cookie;
    xcb_dri3_query_version_reply_t *dri3_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;
    int fd;

    xcb_prefetch_extension_data(xcb_connection, &xcb_dri3_id);

    extension = xcb_get_extension_data(xcb_connection, &xcb_dri3_id);
    if (!(extension && extension->present))
    {
        ERR("DRI3 extension is not present\n");
        return FALSE;
    }

    dri3_cookie = xcb_dri3_query_version(xcb_connection, major, minor);

    dri3_reply = xcb_dri3_query_version_reply(xcb_connection, dri3_cookie, &error);
    if (!dri3_reply)
    {
        free(error);
        ERR("Issue getting requested version of DRI3: %d,%d\n", major, minor);
        return FALSE;
    }

    if (!DRI3Open(dpy, DefaultScreen(dpy), &fd))
    {
        ERR("DRI3 advertised, but not working\n");
        return FALSE;
    }
    close(fd);

    TRACE("DRI3 version %d,%d found. %d %d requested\n", major, minor,
            (int)dri3_reply->major_version, (int)dri3_reply->minor_version);
    free(dri3_reply);

    return TRUE;
}

#ifdef D3D9NINE_DRI2

BOOL DRI2FallbackInit(Display *dpy, struct DRI2priv **priv)
{
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_func;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_func;
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT_func;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_func;
    EGLint major, minor;
    EGLConfig config;
    EGLContext context;
    EGLint i;
    EGLBoolean b;
    EGLenum current_api = 0;
    const char *extensions;
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLint context_compatibility_attribs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };

    current_api = eglQueryAPI();
    eglGetPlatformDisplayEXT_func = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (!eglGetPlatformDisplayEXT_func)
        return FALSE;
    if (!display)
        display = eglGetPlatformDisplayEXT_func(EGL_PLATFORM_X11_EXT, dpy, NULL);
    if (!display)
        return FALSE;
    /* count references on display for multi device setups */
    display_ref++;

    if (eglInitialize(display, &major, &minor) != EGL_TRUE)
        goto clean_egl_display;

    extensions = eglQueryString(display, EGL_CLIENT_APIS);
    if (!extensions || !strstr(extensions, "OpenGL"))
        goto clean_egl_display;

    extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
            !strstr(extensions, "EGL_KHR_create_context") ||
            !strstr(extensions, "EGL_KHR_surfaceless_context") ||
            !strstr(extensions, "EGL_KHR_image_base"))
        goto clean_egl_display;

    if (!eglChooseConfig(display, config_attribs, &config, 1, &i))
        goto clean_egl_display;

    b = eglBindAPI(EGL_OPENGL_API);
    if (b == EGL_FALSE)
        goto clean_egl_display;
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_compatibility_attribs);
    if (context == EGL_NO_CONTEXT)
        goto clean_egl_display;

    glEGLImageTargetTexture2DOES_func = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

    eglCreateImageKHR_func = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");

    eglDestroyImageKHR_func = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");

    if (!eglCreateImageKHR_func ||
            !glEGLImageTargetTexture2DOES_func ||
            !eglDestroyImageKHR_func)
    {
        ERR("eglGetProcAddress failed !");
        goto clean_egl;
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    *priv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct DRI2priv));
    if (!*priv)
        goto clean_egl;
    (*priv)->dpy = dpy;
    (*priv)->display = display;
    (*priv)->context = context;
    (*priv)->glEGLImageTargetTexture2DOES_func = glEGLImageTargetTexture2DOES_func;
    (*priv)->eglCreateImageKHR_func = eglCreateImageKHR_func;
    (*priv)->eglDestroyImageKHR_func = eglDestroyImageKHR_func;
    eglBindAPI(current_api);
    return TRUE;

clean_egl:
    eglDestroyContext(display, context);

clean_egl_display:
    eglTerminate(display);
    eglBindAPI(current_api);
    return FALSE;
}

/* hypothesis: at this step all textures, etc are destroyed */
void DRI2FallbackDestroy(struct DRI2priv *priv)
{
    EGLenum current_api;
    current_api = eglQueryAPI();
    eglBindAPI(EGL_OPENGL_API);
    eglMakeCurrent(priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(priv->display, priv->context);
    if (display)
    {
        /* destroy display connection with last device */
        display_ref--;
        if (!display_ref)
        {
            eglTerminate(display);
            display = NULL;
        }
    }
    eglBindAPI(current_api);
    HeapFree(GetProcessHeap(), 0, priv);
}

BOOL DRI2FallbackCheckSupport(Display *dpy)
{
    struct DRI2priv *priv;
    int fd;
    if (!DRI2FallbackInit(dpy, &priv))
        return FALSE;
    DRI2FallbackDestroy(priv);
    if (!DRI2FallbackOpen(dpy, DefaultScreen(dpy), &fd))
        return FALSE;
    close(fd);
    return TRUE;
}

#endif

BOOL PRESENTCheckExtension(Display *dpy, int major, int minor)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_present_query_version_cookie_t present_cookie;
    xcb_present_query_version_reply_t *present_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;

    xcb_prefetch_extension_data(xcb_connection, &xcb_present_id);

    extension = xcb_get_extension_data(xcb_connection, &xcb_present_id);
    if (!(extension && extension->present))
    {
        ERR("PRESENT extension is not present\n");
        return FALSE;
    }

    present_cookie = xcb_present_query_version(xcb_connection, major, minor);

    present_reply = xcb_present_query_version_reply(xcb_connection, present_cookie, &error);
    if (!present_reply)
    {
        free(error);
        ERR("Issue getting requested version of PRESENT: %d,%d\n", major, minor);
        return FALSE;
    }

    TRACE("PRESENT version %d,%d found. %u %u requested\n", major, minor,
            present_reply->major_version, present_reply->minor_version);

    free(present_reply);

    return TRUE;
}

BOOL DRI3Open(Display *dpy, int screen, int *device_fd)
{
    xcb_dri3_open_cookie_t cookie;
    xcb_dri3_open_reply_t *reply;
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    int fd;
    Window root = RootWindow(dpy, screen);

    cookie = xcb_dri3_open(xcb_connection, root, 0);

    reply = xcb_dri3_open_reply(xcb_connection, cookie, NULL);
    if (!reply)
        return FALSE;

    if (reply->nfd != 1)
    {
        free(reply);
        return FALSE;
    }

    fd = xcb_dri3_open_reply_fds(xcb_connection, reply)[0];
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    *device_fd = fd;
    free(reply);

    return TRUE;
}

#ifdef D3D9NINE_DRI2

static XExtensionInfo _dri2_info_data;
static XExtensionInfo *dri2_info = &_dri2_info_data;
static char dri2_name[] = DRI2_NAME;

#define DRI2CheckExtension(dpy, i, val) \
  XextCheckExtension(dpy, i, dri2_name, val)

static int close_display(Display *dpy, XExtCodes *codes);
static Bool wire_to_event(Display *dpy, XEvent *re, xEvent *event);
static Status event_to_wire(Display *dpy, XEvent *re, xEvent *event);
static int error( Display *dpy, xError *err, XExtCodes *codes, int *ret_code );

static XExtensionHooks dri2_hooks = {
    NULL, /* create_gc */
    NULL, /* copy_gc */
    NULL, /* flush_gc */
    NULL, /* free_gc */
    NULL, /* create_font */
    NULL, /* free_font */
    close_display, /* close_display */
    wire_to_event, /* wire_to_event */
    event_to_wire, /* event_to_wire */
    error, /* error */
    NULL, /* error_string */
};
static XEXT_GENERATE_CLOSE_DISPLAY(close_display, dri2_info);
static XEXT_GENERATE_FIND_DISPLAY(find_display, dri2_info,
                                  dri2_name, &dri2_hooks, 0, NULL);
static Bool wire_to_event(Display *dpy, XEvent *re, xEvent *event)
{
    XExtDisplayInfo *info = find_display(dpy);
    DRI2CheckExtension(dpy, info, False);
    TRACE("dri2 wire_to_event\n");
    return False;
}

static Status event_to_wire(Display *dpy, XEvent *re, xEvent *event)
{
    XExtDisplayInfo *info = find_display(dpy);
    DRI2CheckExtension(dpy, info, False);
    TRACE("dri2 event_to_wire\n");
    return False;
}

static int error(Display *dpy, xError *err, XExtCodes *codes, int *ret_code)
{
    TRACE("dri2 error\n");
    return False;
}

#define XALIGN(x) (((x) + 3) & (~3))

static BOOL DRI2Connect(Display *dpy, XID window, unsigned driver_type, char **device)
{
    XExtDisplayInfo *info = find_display(dpy);
    xDRI2ConnectReply rep;
    xDRI2ConnectReq *req;
    int dev_len, driv_len;
    char *driver;

    DRI2CheckExtension(dpy, info, False);

    *device = NULL;

    LockDisplay(dpy);
    GetReq(DRI2Connect, req);
    req->reqType = info->codes->major_opcode;
    req->dri2ReqType = X_DRI2Connect;
    req->window = window;
    req->driverType = driver_type;
    if (!_XReply(dpy, (xReply *)&rep, 0, xFalse))
    {
        UnlockDisplay(dpy);
        SyncHandle();
        return False;
    }

    /* check string lengths */
    dev_len = rep.deviceNameLength;
    driv_len = rep.driverNameLength;
    if (dev_len == 0 || driv_len == 0)
    {
        _XEatData(dpy, XALIGN(dev_len) + XALIGN(driv_len));
        UnlockDisplay(dpy);
        SyncHandle();
        return False;
    }

    /* read out driver */
    driver = HeapAlloc(GetProcessHeap(), 0, driv_len + 1);
    if (!driver)
    {
        _XEatData(dpy, XALIGN(dev_len) + XALIGN(driv_len));
        UnlockDisplay(dpy);
        SyncHandle();
        return False;
    }
    _XReadPad(dpy, driver, driv_len);
    HeapFree(GetProcessHeap(), 0, driver); /* we don't need the driver */

    /* read out device */
    *device = HeapAlloc(GetProcessHeap(), 0, dev_len + 1);
    if (!*device)
    {
        _XEatData(dpy, XALIGN(dev_len));
        UnlockDisplay(dpy);
        SyncHandle();
        return False;
    }
    _XReadPad(dpy, *device, dev_len);
    (*device)[dev_len] = '\0';

    UnlockDisplay(dpy);
    SyncHandle();

    return True;
}

static Bool DRI2Authenticate(Display *dpy, XID window, uint32_t token)
{
    XExtDisplayInfo *info = find_display(dpy);
    xDRI2AuthenticateReply rep;
    xDRI2AuthenticateReq *req;

    DRI2CheckExtension(dpy, info, False);

    LockDisplay(dpy);
    GetReq(DRI2Authenticate, req);
    req->reqType = info->codes->major_opcode;
    req->dri2ReqType = X_DRI2Authenticate;
    req->window = window;
    req->magic = token;
    if (!_XReply(dpy, (xReply *)&rep, 0, xFalse))
    {
        UnlockDisplay(dpy);
        SyncHandle();
        return False;
    }
    UnlockDisplay(dpy);
    SyncHandle();

    return rep.authenticated ? True : False;
}

BOOL DRI2FallbackOpen(Display *dpy, int screen, int *device_fd)
{
    char *device;
    int fd;
    Window root = RootWindow(dpy, screen);
    drm_auth_t auth;

    if (!DRI2Connect(dpy, root, DRI2DriverDRI, &device))
        return FALSE;

    fd = open(device, O_RDWR);
    HeapFree(GetProcessHeap(), 0, device);
    if (fd < 0)
        return FALSE;

    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth) != 0)
    {
        close(fd);
        return FALSE;
    }

    if (!DRI2Authenticate(dpy, root, auth.magic))
    {
        close(fd);
        return FALSE;
    }

    *device_fd = fd;

    return TRUE;
}

#endif


BOOL DRI3PixmapFromDmaBuf(Display *dpy, int screen, int fd, int width, int height,
        int stride, int depth, int bpp, Pixmap *pixmap)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    Window root = RootWindow(dpy, screen);
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;

    cookie = xcb_dri3_pixmap_from_buffer_checked(xcb_connection,
            (*pixmap = xcb_generate_id(xcb_connection)), root, 0,
            width, height, stride, depth, bpp, fd);

    error = xcb_request_check(xcb_connection, cookie); /* performs a flush */
    if (error)
    {
        ERR("Error using DRI3 to convert a DmaBufFd to pixmap\n");
        return FALSE;
    }
    return TRUE;
}

BOOL DRI3DmaBufFromPixmap(Display *dpy, Pixmap pixmap, int *fd, int *width, int *height,
        int *stride, int *depth, int *bpp)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_dri3_buffer_from_pixmap_cookie_t bp_cookie;
    xcb_dri3_buffer_from_pixmap_reply_t  *bp_reply;

    bp_cookie = xcb_dri3_buffer_from_pixmap(xcb_connection, pixmap);
    bp_reply = xcb_dri3_buffer_from_pixmap_reply(xcb_connection, bp_cookie, NULL);
    if (!bp_reply)
        return FALSE;
    *fd = xcb_dri3_buffer_from_pixmap_reply_fds(xcb_connection, bp_reply)[0];
    *width = bp_reply->width;
    *height = bp_reply->height;
    *stride = bp_reply->stride;
    *depth = bp_reply->depth;
    *bpp = bp_reply->depth;
    return TRUE;
}

static PRESENTPixmapPriv *PRESENTFindPixmapPriv(PRESENTpriv *present_priv, uint32_t serial)
{
    PRESENTPixmapPriv *current = present_priv->first_present_priv;

    while (current)
    {
        if (current->serial == serial)
            return current;
        current = current->next;
    }
    return NULL;
}

static void PRESENThandle_events(PRESENTpriv *present_priv, xcb_present_generic_event_t *ge)
{
    PRESENTPixmapPriv *present_pixmap_priv = NULL;

    switch (ge->evtype)
    {
        case XCB_PRESENT_COMPLETE_NOTIFY:
        {
            xcb_present_complete_notify_event_t *ce = (void *) ge;
            if (ce->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC)
            {
                if (ce->serial)
                    present_priv->notify_with_serial_pending = FALSE;
                free(ce);
                return;
            }
            present_pixmap_priv = PRESENTFindPixmapPriv(present_priv, ce->serial);
            if (!present_pixmap_priv || ce->kind != XCB_PRESENT_COMPLETE_KIND_PIXMAP)
            {
                ERR("FATAL ERROR: PRESENT handling failed\n");
                free(ce);
                return;
            }
            present_pixmap_priv->present_complete_pending--;
            switch (ce->mode)
            {
                case XCB_PRESENT_COMPLETE_MODE_FLIP:
                    present_pixmap_priv->last_present_was_flip = TRUE;
                    break;
                case XCB_PRESENT_COMPLETE_MODE_COPY:
                    present_pixmap_priv->last_present_was_flip = FALSE;
                    break;
            }
            present_priv->pixmap_present_pending--;
            present_priv->last_msc = ce->msc;
            break;
        }
        case XCB_PRESENT_EVENT_IDLE_NOTIFY:
        {
            xcb_present_idle_notify_event_t *ie = (void *) ge;
            present_pixmap_priv = PRESENTFindPixmapPriv(present_priv, ie->serial);
            if (!present_pixmap_priv || present_pixmap_priv->pixmap != ie->pixmap)
            {
                ERR("FATAL ERROR: PRESENT handling failed\n");
                free(ie);
                return;
            }
            present_pixmap_priv->released = TRUE;
            present_priv->idle_notify_since_last_check = TRUE;
            break;
        }
    }
    free(ge);
}

static void PRESENTflush_events(PRESENTpriv *present_priv, BOOL assert_no_other_thread_waiting)
{
    xcb_generic_event_t *ev;

    if ((present_priv->xcb_wait && !assert_no_other_thread_waiting) || /* don't steal events to someone waiting */
            !present_priv->special_event)
        return;

    while ((ev = xcb_poll_for_special_event(present_priv->xcb_connection,
            present_priv->special_event)) != NULL)
    {
        PRESENThandle_events(present_priv, (void *) ev);
    }
}

static BOOL PRESENTwait_events(PRESENTpriv *present_priv, BOOL allow_other_threads)
{
    xcb_generic_event_t *ev;

    if (allow_other_threads)
    {
        present_priv->xcb_wait = TRUE;
        EnterCriticalSection(&present_priv->mutex_xcb_wait);
        LeaveCriticalSection(&present_priv->mutex_present);
    }
    ev = xcb_wait_for_special_event(present_priv->xcb_connection, present_priv->special_event);
    if (allow_other_threads)
    {
        LeaveCriticalSection(&present_priv->mutex_xcb_wait);
        EnterCriticalSection(&present_priv->mutex_present);
        present_priv->xcb_wait = FALSE;
    }
    if (!ev)
    {
        ERR("FATAL error: xcb had an error\n");
        return FALSE;
    }

    PRESENThandle_events(present_priv, (void *) ev);
    return TRUE;
}

static struct xcb_connection_t *create_xcb_connection(Display *dpy)
{
    int screen_num = DefaultScreen(dpy);
    xcb_connection_t *ret;
    xcb_xfixes_query_version_cookie_t cookie;
    xcb_xfixes_query_version_reply_t *rep;

    ret = xcb_connect(DisplayString(dpy), &screen_num);
    cookie = xcb_xfixes_query_version_unchecked(ret, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
    rep = xcb_xfixes_query_version_reply(ret, cookie, NULL);
    if (rep)
        free(rep);
    return ret;
}

BOOL PRESENTInit(Display *dpy, PRESENTpriv **present_priv)
{
    *present_priv = (PRESENTpriv *) HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(PRESENTpriv));

    if (!*present_priv)
        return FALSE;

    (*present_priv)->xcb_connection = create_xcb_connection(dpy);
    (*present_priv)->xcb_connection_bis = create_xcb_connection(dpy);

    InitializeCriticalSection(&(*present_priv)->mutex_present);
    InitializeCriticalSection(&(*present_priv)->mutex_xcb_wait);
    return TRUE;
}

static void PRESENTForceReleases(PRESENTpriv *present_priv)
{
    PRESENTPixmapPriv *current = NULL;

    if (!present_priv->window)
        return;

    /* There should be no other thread listening for events here.
     * This can happen when hDestWindowOverride changes without reset.
     * This case should never happen, but can happen in theory.*/
    if (present_priv->xcb_wait)
    {
        xcb_present_notify_msc(present_priv->xcb_connection, present_priv->window, 0, 0, 0, 0);
        xcb_flush(present_priv->xcb_connection);
        EnterCriticalSection(&present_priv->mutex_xcb_wait);
        LeaveCriticalSection(&present_priv->mutex_xcb_wait);
        /* the problem here is that we don't have access to the event the other thread got.
         * It is either presented event, idle event or notify event.
         */
        while (present_priv->pixmap_present_pending >= 2)
            PRESENTwait_events(present_priv, FALSE);
        PRESENTflush_events(present_priv, TRUE);
        /* Remaining events to come can be a pair of present/idle,
         * or an idle, or nothing. To be sure we are after all pixmaps
         * have been presented, add an event to the queue that can only
         * be after the present event, then if we receive an event more,
         * we are sure all pixmaps were presented */
        present_priv->notify_with_serial_pending = TRUE;
        xcb_present_notify_msc(present_priv->xcb_connection, present_priv->window,
                1, present_priv->last_target + 5, 0, 0);

        xcb_flush(present_priv->xcb_connection);
        while (present_priv->notify_with_serial_pending)
            PRESENTwait_events(present_priv, FALSE);
        /* Now we are sure we are not expecting any new event */
    }
    else
    {
        while (present_priv->pixmap_present_pending) /* wait all sent pixmaps are presented */
            PRESENTwait_events(present_priv, FALSE);
        PRESENTflush_events(present_priv, TRUE); /* may be remaining idle event */
        /* Since idle events are send with the complete events when it is not flips,
         * we are not expecting any new event here */
    }

    current = present_priv->first_present_priv;
    while (current)
    {
        if (!current->released)
        {
            if (!current->last_present_was_flip && !present_priv->xcb_wait)
            {
                ERR("ERROR: a pixmap seems not released by PRESENT for no reason. Code bug.\n");
            }
            else
            {
                /* Present the same pixmap with a non-valid part to force the copy mode and the releases */
                xcb_xfixes_region_t valid, update;
                xcb_rectangle_t rect_update;
                rect_update.x = 0;
                rect_update.y = 0;
                rect_update.width = 8;
                rect_update.height = 1;
                valid = xcb_generate_id(present_priv->xcb_connection);
                update = xcb_generate_id(present_priv->xcb_connection);
                xcb_xfixes_create_region(present_priv->xcb_connection, valid, 1, &rect_update);
                xcb_xfixes_create_region(present_priv->xcb_connection, update, 1, &rect_update);
                /* here we know the pixmap has been presented. Thus if it is on screen,
                 * the following request can only make it released by the server if it is not */
                xcb_present_pixmap(present_priv->xcb_connection, present_priv->window,
                        current->pixmap, 0, valid, update, 0, 0, None, None,
                        None, XCB_PRESENT_OPTION_COPY | XCB_PRESENT_OPTION_ASYNC, 0, 0, 0, 0, NULL);
                xcb_flush(present_priv->xcb_connection);
                PRESENTwait_events(present_priv, FALSE); /* by assumption this can only be idle event */
                PRESENTflush_events(present_priv, TRUE); /* Shoudln't be needed */
            }
        }
        current = current->next;
    }
    /* Now all pixmaps are released (possibility if xcb_wait is true that one is not aware yet),
     * and we don't expect any new Present event to come from Xserver */
}

static void PRESENTFreeXcbQueue(PRESENTpriv *present_priv)
{
    if (present_priv->window)
    {
        xcb_unregister_for_special_event(present_priv->xcb_connection, present_priv->special_event);
        present_priv->last_msc = 0;
        present_priv->last_target = 0;
        present_priv->special_event = NULL;
    }
}

static BOOL PRESENTPrivChangeWindow(PRESENTpriv *present_priv, XID window)
{
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    xcb_present_event_t eid;

    PRESENTForceReleases(present_priv);
    PRESENTFreeXcbQueue(present_priv);
    present_priv->window = window;

    if (window)
    {
        cookie = xcb_present_select_input_checked(present_priv->xcb_connection,
                (eid = xcb_generate_id(present_priv->xcb_connection)), window,
                XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);

        present_priv->special_event = xcb_register_for_special_xge(present_priv->xcb_connection,
                &xcb_present_id, eid, NULL);

        error = xcb_request_check(present_priv->xcb_connection, cookie); /* performs a flush */
        if (error || !present_priv->special_event)
        {
            ERR("FAILED to use the X PRESENT extension. Was the destination a window ?\n");
            if (present_priv->special_event)
                xcb_unregister_for_special_event(present_priv->xcb_connection, present_priv->special_event);
            present_priv->special_event = NULL;
            present_priv->window = 0;
        }
    }
    return (present_priv->window != 0);
}

/* Destroy the content, except the link and the struct mem */
static void PRESENTDestroyPixmapContent(Display *dpy, PRESENTPixmapPriv *present_pixmap)
{
    XFreePixmap(dpy, present_pixmap->pixmap);
#ifdef D3D9NINE_DRI2
    if (present_pixmap->dri2_info.is_dri2)
    {
        struct DRI2priv *dri2_priv = present_pixmap->dri2_info.dri2_priv;
        EGLenum current_api;
        current_api = eglQueryAPI();
        eglBindAPI(EGL_OPENGL_API);
        if (eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, dri2_priv->context))
        {
            glDeleteFramebuffers(1, &present_pixmap->dri2_info.fbo_read);
            glDeleteFramebuffers(1, &present_pixmap->dri2_info.fbo_write);
            glDeleteTextures(1, &present_pixmap->dri2_info.texture_read);
            glDeleteTextures(1, &present_pixmap->dri2_info.texture_write);
        }
        else
            ERR("eglMakeCurrent failed with 0x%0X\n", eglGetError());

        eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglBindAPI(current_api);
    }
#endif
}

void PRESENTDestroy(Display *dpy, PRESENTpriv *present_priv)
{
    PRESENTPixmapPriv *current = NULL;

    EnterCriticalSection(&present_priv->mutex_present);

    PRESENTForceReleases(present_priv);

    current = present_priv->first_present_priv;
    while (current)
    {
        PRESENTPixmapPriv *next = current->next;
        PRESENTDestroyPixmapContent(dpy, current);
        HeapFree(GetProcessHeap(), 0, current);
        current = next;
    }

    PRESENTFreeXcbQueue(present_priv);

    xcb_disconnect(present_priv->xcb_connection);
    xcb_disconnect(present_priv->xcb_connection_bis);
    LeaveCriticalSection(&present_priv->mutex_present);
    DeleteCriticalSection(&present_priv->mutex_present);
    DeleteCriticalSection(&present_priv->mutex_xcb_wait);

    HeapFree(GetProcessHeap(), 0, present_priv);
}

BOOL PRESENTPixmapInit(PRESENTpriv *present_priv, Pixmap pixmap, PRESENTPixmapPriv **present_pixmap_priv)
{
    xcb_get_geometry_cookie_t cookie;
    xcb_get_geometry_reply_t *reply;

    cookie = xcb_get_geometry(present_priv->xcb_connection, pixmap);
    reply = xcb_get_geometry_reply(present_priv->xcb_connection, cookie, NULL);

    if (!reply)
        return FALSE;

    *present_pixmap_priv = (PRESENTPixmapPriv *) HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(PRESENTPixmapPriv));

    if (!*present_pixmap_priv)
    {
        free(reply);
        return FALSE;
    }
    EnterCriticalSection(&present_priv->mutex_present);

    (*present_pixmap_priv)->released = TRUE;
    (*present_pixmap_priv)->pixmap = pixmap;
    (*present_pixmap_priv)->present_priv = present_priv;
    (*present_pixmap_priv)->next = present_priv->first_present_priv;
    (*present_pixmap_priv)->width = reply->width;
    (*present_pixmap_priv)->height = reply->height;
    (*present_pixmap_priv)->depth = reply->depth;
#ifdef D3D9NINE_DRI2
    (*present_pixmap_priv)->dri2_info.is_dri2 = FALSE;
#endif
    free(reply);

    present_priv->last_serial_given++;
    (*present_pixmap_priv)->serial = present_priv->last_serial_given;
    present_priv->first_present_priv = *present_pixmap_priv;

    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
}

#ifdef D3D9NINE_DRI2

BOOL DRI2FallbackPRESENTPixmap(PRESENTpriv *present_priv, struct DRI2priv *dri2_priv,
        int fd, int width, int height, int stride, int depth,
        int bpp, PRESENTPixmapPriv **present_pixmap_priv)
{
    Window root = RootWindow(dri2_priv->dpy, DefaultScreen(dri2_priv->dpy));
    Pixmap pixmap;
    EGLImageKHR image;
    GLuint texture_read, texture_write, fbo_read, fbo_write;
    EGLint attribs[] = {
        EGL_WIDTH, 0,
        EGL_HEIGHT, 0,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, 0,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, 0,
        EGL_NONE
    };
    EGLenum current_api = 0;
    int status;

    EnterCriticalSection(&present_priv->mutex_present);

    pixmap = XCreatePixmap(dri2_priv->dpy, root, width, height, 24);
    if (!pixmap)
        goto fail;

    attribs[1] = width;
    attribs[3] = height;
    attribs[7] = fd;
    attribs[11] = stride;

    current_api = eglQueryAPI();
    eglBindAPI(EGL_OPENGL_API);

    /* We bind the dma-buf to a EGLImage, then to a texture, and then to a fbo.
     * Note that we can delete the EGLImage, but we shouldn't delete the texture,
     * else the fbo is invalid */

    image = dri2_priv->eglCreateImageKHR_func(dri2_priv->display,
            EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

    if (image == EGL_NO_IMAGE_KHR)
        goto fail;
    close(fd);

    if (eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, dri2_priv->context))
    {
        glGenTextures(1, &texture_read);
        glBindTexture(GL_TEXTURE_2D, texture_read);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        dri2_priv->glEGLImageTargetTexture2DOES_func(GL_TEXTURE_2D, image);
        glGenFramebuffers(1, &fbo_read);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_read);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture_read,
                               0);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            goto fail;
        glBindTexture(GL_TEXTURE_2D, 0);
        dri2_priv->eglDestroyImageKHR_func(dri2_priv->display, image);

        /* We bind a newly created pixmap (to which we want to copy the content)
         * to an EGLImage, then to a texture, then to a fbo. */
        image = dri2_priv->eglCreateImageKHR_func(dri2_priv->display,
                                                  dri2_priv->context,
                                                  EGL_NATIVE_PIXMAP_KHR,
                                                  (void *)pixmap, NULL);
        if (image == EGL_NO_IMAGE_KHR)
            goto fail;

        glGenTextures(1, &texture_write);
        glBindTexture(GL_TEXTURE_2D, texture_write);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        dri2_priv->glEGLImageTargetTexture2DOES_func(GL_TEXTURE_2D, image);
        glGenFramebuffers(1, &fbo_write);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_write);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture_write,
                               0);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            goto fail;
        glBindTexture(GL_TEXTURE_2D, 0);
        dri2_priv->eglDestroyImageKHR_func(dri2_priv->display, image);
    }
    else
        ERR("eglMakeCurrent failed with 0x%0X\n", eglGetError());

    eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    *present_pixmap_priv = (PRESENTPixmapPriv *) HeapAlloc(GetProcessHeap(),
             HEAP_ZERO_MEMORY, sizeof(PRESENTPixmapPriv));

    if (!*present_pixmap_priv)
        goto fail;

    (*present_pixmap_priv)->released = TRUE;
    (*present_pixmap_priv)->pixmap = pixmap;
    (*present_pixmap_priv)->present_priv = present_priv;
    (*present_pixmap_priv)->next = present_priv->first_present_priv;
    (*present_pixmap_priv)->width = width;
    (*present_pixmap_priv)->height = height;
    (*present_pixmap_priv)->depth = depth;
    (*present_pixmap_priv)->dri2_info.is_dri2 = TRUE;
    (*present_pixmap_priv)->dri2_info.dri2_priv = dri2_priv;
    (*present_pixmap_priv)->dri2_info.fbo_read = fbo_read;
    (*present_pixmap_priv)->dri2_info.fbo_write = fbo_write;
    (*present_pixmap_priv)->dri2_info.texture_read = texture_read;
    (*present_pixmap_priv)->dri2_info.texture_write = texture_write;

    present_priv->last_serial_given++;
    (*present_pixmap_priv)->serial = present_priv->last_serial_given;
    present_priv->first_present_priv = *present_pixmap_priv;

    eglBindAPI(current_api);

    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
fail:
    eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglBindAPI(current_api);
    LeaveCriticalSection(&present_priv->mutex_present);
    return FALSE;
}

#endif

BOOL PRESENTTryFreePixmap(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    PRESENTPixmapPriv *current;

    EnterCriticalSection(&present_priv->mutex_present);

    if (!present_pixmap_priv->released || present_pixmap_priv->present_complete_pending)
    {
        LeaveCriticalSection(&present_priv->mutex_present);
        return FALSE;
    }

    if (present_priv->first_present_priv == present_pixmap_priv)
    {
        present_priv->first_present_priv = present_pixmap_priv->next;
        goto free_priv;
    }

    current = present_priv->first_present_priv;
    while (current->next != present_pixmap_priv)
        current = current->next;
    current->next = present_pixmap_priv->next;
free_priv:
    PRESENTDestroyPixmapContent(dpy, present_pixmap_priv);
    HeapFree(GetProcessHeap(), 0, present_pixmap_priv);
    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
}

BOOL PRESENTHelperCopyFront(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    uint32_t v = 0;
    xcb_gcontext_t gc;

    EnterCriticalSection(&present_priv->mutex_present);

    if (!present_priv->window)
    {
        LeaveCriticalSection(&present_priv->mutex_present);
        return FALSE;
    }

    gc = xcb_generate_id(present_priv->xcb_connection);
    xcb_create_gc(present_priv->xcb_connection, gc, present_priv->window,
             XCB_GC_GRAPHICS_EXPOSURES, &v);

    cookie = xcb_copy_area_checked(present_priv->xcb_connection,
             present_priv->window, present_pixmap_priv->pixmap, gc,
             0, 0, 0, 0, present_pixmap_priv->width, present_pixmap_priv->height);

    error = xcb_request_check(present_priv->xcb_connection, cookie);
    xcb_free_gc(present_priv->xcb_connection, gc);
    LeaveCriticalSection(&present_priv->mutex_present);
    return (error != NULL);
}

BOOL PRESENTPixmap(Display *dpy, XID window, PRESENTPixmapPriv *present_pixmap_priv,
        const UINT PresentationInterval, const BOOL PresentAsync, const BOOL SwapEffectCopy,
        const RECT *pSourceRect, const RECT *pDestRect, const RGNDATA *pDirtyRegion)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
#ifdef D3D9NINE_DRI2
    struct DRI2priv *dri2_priv = present_pixmap_priv->dri2_info.dri2_priv;
    EGLenum current_api = 0;
#endif
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    int64_t target_msc, presentationInterval;
    xcb_xfixes_region_t valid, update;
    int16_t x_off, y_off;
    uint32_t options = XCB_PRESENT_OPTION_NONE;

    EnterCriticalSection(&present_priv->mutex_present);

    if (window != present_priv->window)
        PRESENTPrivChangeWindow(present_priv, window);

    if (!window)
    {
        ERR("ERROR: Try to Present a pixmap on a NULL window\n");
        LeaveCriticalSection(&present_priv->mutex_present);
        return FALSE;
    }

    PRESENTflush_events(present_priv, FALSE);
    /* Note: present_pixmap_priv->present_complete_pending may be non-0, because
     * on some paths the Xserver sends the complete event just after the idle
     * event. */
    if (!present_pixmap_priv->released)
    {
        ERR("FATAL ERROR: Trying to Present a pixmap not released\n");
        LeaveCriticalSection(&present_priv->mutex_present);
        return FALSE;
    }
#ifdef D3D9NINE_DRI2
    if (present_pixmap_priv->dri2_info.is_dri2)
    {
        current_api = eglQueryAPI();
        eglBindAPI(EGL_OPENGL_API);
        if (eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, dri2_priv->context))
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, present_pixmap_priv->dri2_info.fbo_read);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, present_pixmap_priv->dri2_info.fbo_write);

            glBlitFramebuffer(0, 0, present_pixmap_priv->width, present_pixmap_priv->height,
                              0, 0, present_pixmap_priv->width, present_pixmap_priv->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glFlush(); /* Perhaps useless */
        }
        else
            ERR("eglMakeCurrent failed with 0x%0X\n", eglGetError());

        eglMakeCurrent(dri2_priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglBindAPI(current_api);
    }
#endif
    target_msc = present_priv->last_msc;

    presentationInterval = PresentationInterval;
    if (PresentAsync)
        options |= XCB_PRESENT_OPTION_ASYNC;
    if (SwapEffectCopy)
        options |= XCB_PRESENT_OPTION_COPY;

    target_msc += presentationInterval * (present_priv->pixmap_present_pending + 1);

    /* Note: PRESENT defines some way to do partial copy:
     * presentproto:
     * 'x-off' and 'y-off' define the location in the window where
     *  the 0,0 location of the pixmap will be presented. valid-area
     *  and update-area are relative to the pixmap.
     */
    if (!pSourceRect && !pDestRect && !pDirtyRegion)
    {
        valid = 0;
        update = 0;
        x_off = 0;
        y_off = 0;
    }
    else
    {
        xcb_rectangle_t rect_update;
        xcb_rectangle_t *rect_updates;
        int i;

        rect_update.x = 0;
        rect_update.y = 0;
        rect_update.width = present_pixmap_priv->width;
        rect_update.height = present_pixmap_priv->height;
        x_off = 0;
        y_off = 0;
        if (pSourceRect)
        {
            x_off = -pSourceRect->left;
            y_off = -pSourceRect->top;
            rect_update.x = pSourceRect->left;
            rect_update.y = pSourceRect->top;
            rect_update.width = pSourceRect->right - pSourceRect->left;
            rect_update.height = pSourceRect->bottom - pSourceRect->top;
        }
        if (pDestRect)
        {
            x_off += pDestRect->left;
            y_off += pDestRect->top;
            rect_update.width = pDestRect->right - pDestRect->left;
            rect_update.height = pDestRect->bottom - pDestRect->top;
            /* Note: the size of pDestRect and pSourceRect are supposed to be the same size
             * because the driver would have done things to assure that. */
        }
        valid = xcb_generate_id(present_priv->xcb_connection_bis);
        update = xcb_generate_id(present_priv->xcb_connection_bis);
        xcb_xfixes_create_region(present_priv->xcb_connection_bis, valid, 1, &rect_update);
        if (pDirtyRegion && pDirtyRegion->rdh.nCount)
        {
            rect_updates = (void *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    sizeof(xcb_rectangle_t) * pDirtyRegion->rdh.nCount);

            for (i = 0; i < pDirtyRegion->rdh.nCount; i++)
            {
                RECT rc;
                memcpy(&rc, pDirtyRegion->Buffer + i * sizeof(RECT), sizeof(RECT));
                rect_update.x = rc.left;
                rect_update.y = rc.top;
                rect_update.width = rc.right - rc.left;
                rect_update.height = rc.bottom - rc.top;
                memcpy(rect_updates + i * sizeof(xcb_rectangle_t), &rect_update, sizeof(xcb_rectangle_t));
            }
            xcb_xfixes_create_region(present_priv->xcb_connection_bis, update, pDirtyRegion->rdh.nCount, rect_updates);
            HeapFree(GetProcessHeap(), 0, rect_updates);
        } else
            xcb_xfixes_create_region(present_priv->xcb_connection_bis, update, 1, &rect_update);
    }

    cookie = xcb_present_pixmap_checked(present_priv->xcb_connection_bis,
            window, present_pixmap_priv->pixmap, present_pixmap_priv->serial,
            valid, update, x_off, y_off, None, None, None, options,
            target_msc, 0, 0, 0, NULL);
    error = xcb_request_check(present_priv->xcb_connection_bis, cookie); /* performs a flush */

    if (update)
        xcb_xfixes_destroy_region(present_priv->xcb_connection_bis, update);
    if (valid)
        xcb_xfixes_destroy_region(present_priv->xcb_connection_bis, valid);

    if (error)
    {
        xcb_get_geometry_cookie_t cookie_geom;
        xcb_get_geometry_reply_t *reply;

        cookie_geom = xcb_get_geometry(present_priv->xcb_connection_bis, window);
        reply = xcb_get_geometry_reply(present_priv->xcb_connection_bis, cookie_geom, NULL);

        ERR("Error using PRESENT. Here some debug info\n");
        if (!reply)
        {
            ERR("Error querying window info. Perhaps it doesn't exist anymore\n");
            LeaveCriticalSection(&present_priv->mutex_present);
            return FALSE;
        }
        ERR("Pixmap: width=%d, height=%d, depth=%d\n",
                present_pixmap_priv->width, present_pixmap_priv->height,
                present_pixmap_priv->depth);

        ERR("Window: width=%d, height=%d, depth=%d, x=%d, y=%d\n",
                (int) reply->width, (int) reply->height,
                (int) reply->depth, (int) reply->x, (int) reply->y);

        ERR("Present parameter: PresentationInterval=%d, Pending presentations=%d\n",
                PresentationInterval, present_priv->pixmap_present_pending);

        if (present_pixmap_priv->depth != reply->depth)
            ERR("Depths are different. PRESENT needs the pixmap and the window have same depth\n");
        free(reply);
        LeaveCriticalSection(&present_priv->mutex_present);
        return FALSE;
    }
    present_priv->last_target = target_msc;
    present_priv->pixmap_present_pending++;
    present_pixmap_priv->present_complete_pending++;
    present_pixmap_priv->released = FALSE;
    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
}

BOOL PRESENTWaitPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;

    EnterCriticalSection(&present_priv->mutex_present);

    PRESENTflush_events(present_priv, FALSE);

    /* The part with present_pixmap_priv->present_complete_pending is legacy behaviour.
     * It matters for SwapEffectCopy with swapinterval > 0. */
    while (!present_pixmap_priv->released || present_pixmap_priv->present_complete_pending)
    {
        /* Note: following if should not happen because we'll never
         * use two PRESENTWaitPixmapReleased in parallels on same window.
         * However it would make it work in that case */
        if (present_priv->xcb_wait)
        {
            /* we allow only one thread to dispatch events */
            EnterCriticalSection(&present_priv->mutex_xcb_wait);
            /* here the other thread got an event but hasn't treated it yet */
            LeaveCriticalSection(&present_priv->mutex_xcb_wait);
            LeaveCriticalSection(&present_priv->mutex_present);
            Sleep(10); /* Let it treat the event */
            EnterCriticalSection(&present_priv->mutex_present);
        }
        else if (!PRESENTwait_events(present_priv, TRUE))
        {
            LeaveCriticalSection(&present_priv->mutex_present);
            return FALSE;
        }
    }
    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
}

BOOL PRESENTIsPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    BOOL ret;

    EnterCriticalSection(&present_priv->mutex_present);

    PRESENTflush_events(present_priv, FALSE);

    ret = present_pixmap_priv->released;

    LeaveCriticalSection(&present_priv->mutex_present);
    return ret;
}

BOOL PRESENTWaitReleaseEvent(PRESENTpriv *present_priv)
{

    EnterCriticalSection(&present_priv->mutex_present);

    while (!present_priv->idle_notify_since_last_check)
    {
        /* Note: following if should not happen because we'll never
         * use two PRESENTWaitPixmapReleased in parallels on same window.
         * However it would make it work in that case */
        if (present_priv->xcb_wait)
        {
            /* we allow only one thread to dispatch events */
            EnterCriticalSection(&present_priv->mutex_xcb_wait);
            /* here the other thread got an event but hasn't treated it yet */
            LeaveCriticalSection(&present_priv->mutex_xcb_wait);
            LeaveCriticalSection(&present_priv->mutex_present);
            Sleep(10); /* Let it treat the event */
            EnterCriticalSection(&present_priv->mutex_present);
        }
        else if (!PRESENTwait_events(present_priv, TRUE))
        {
            ERR("Issue in PRESENTWaitReleaseEvent\n");
            LeaveCriticalSection(&present_priv->mutex_present);
            return FALSE;
        }
    }
    present_priv->idle_notify_since_last_check = FALSE;

    LeaveCriticalSection(&present_priv->mutex_present);
    return TRUE;
}
