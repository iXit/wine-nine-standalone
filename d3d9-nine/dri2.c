/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine DRI2 interface
 *
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015-2019 Patrick Rudolph
 */

#ifdef D3D9NINE_DRI2

#include <windows.h>
#include <wine/debug.h>
#include <sys/ioctl.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri2.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

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

#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "backend.h"
#include "xcb_present.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

const char * const lib_egl = "libEGL.so.1";

static EGLDisplay display = NULL;
static int display_ref = 0;

struct dri2_pixmap_priv {
    GLuint fbo_read;
    GLuint fbo_write;
    GLuint texture_read;
    GLuint texture_write;
    unsigned int width;
    unsigned int height;
    struct dri2_pixmap_priv *next;
};

struct dri2_priv {
    struct dri2_pixmap_priv *first_dri2_priv;
    Display *dpy;
    int screen;
    int fd;
    EGLDisplay display;
    EGLContext context;
    void *h_egl;

    /* egl */
    void *(*eglGetProcAddress)(const char *procname);
    EGLContext (*eglCreateContext)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
    EGLBoolean (*eglDestroyContext)(EGLDisplay dpy, EGLContext ctx);
    EGLint (*eglGetError)(void);
    EGLBoolean (*eglInitialize)(EGLDisplay dpy, EGLint *major, EGLint *minor);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    const char *(*eglQueryString)(EGLDisplay dpy, EGLint name);
    EGLBoolean (*eglTerminate)(EGLDisplay dpy);
    EGLBoolean (*eglChooseConfig)(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
    EGLBoolean (*eglBindAPI)(EGLenum api);
    EGLenum (*eglQueryAPI)(void);

    /* eglext */
    EGLImageKHR (*eglCreateImageKHR)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
    EGLBoolean (*eglDestroyImageKHR)(EGLDisplay dpy, EGLImageKHR image);
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);

    /* gl */
    void (*glFlush)(void);
    void (*glTexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*glGenTextures)(GLsizei n, GLuint *textures);
    void (*glDeleteTextures)(GLsizei n, const GLuint *textures);
    void (*glBindTexture)(GLenum target, GLuint texture);
    void (*glEGLImageTargetTexture2DOES)(GLenum target, GLeglImageOES image);

    /* glext */
    void (*glBindFramebuffer)(GLenum target, GLuint framebuffer);
    void (*glDeleteFramebuffers)(GLsizei n, const GLuint *framebuffers);
    void (*glGenFramebuffers)(GLsizei n, GLuint *framebuffers);
    GLenum (*glCheckFramebufferStatus)(GLenum target);
    void (*glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void (*glBlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
};

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
    WINE_TRACE("dri2 wire_to_event\n");
    return False;
}

static Status event_to_wire(Display *dpy, XEvent *re, xEvent *event)
{
    XExtDisplayInfo *info = find_display(dpy);
    DRI2CheckExtension(dpy, info, False);
    WINE_TRACE("dri2 event_to_wire\n");
    return False;
}

static int error(Display *dpy, xError *err, XExtCodes *codes, int *ret_code)
{
    WINE_TRACE("dri2 error\n");
    return False;
}

#define XALIGN(x) (((x) + 3) & (~3))

static BOOL dri2_connect(Display *dpy, XID window, unsigned driver_type, char **device)
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

static Bool dri2_authenticate(Display *dpy, XID window, uint32_t token)
{
    xcb_generic_error_t *auth_error = NULL;
    xcb_dri2_authenticate_cookie_t cookie;
    xcb_connection_t *conn = XGetXCBConnection(dpy);

    cookie = xcb_dri2_authenticate(conn, window, token);
    Bool authenticated;

    xcb_dri2_authenticate_reply_t *reply =
        xcb_dri2_authenticate_reply(conn, cookie, &auth_error);
    if (auth_error) {
        free(auth_error);
        return FALSE;
    }
    if (!reply) {
        return FALSE;
    }

    authenticated = reply->authenticated;
    free(reply);

    return authenticated;
}

static void *dri2_eglGetProcAddress(struct dri2_priv *priv, const char *procname)
{
    void *p;

    p = dlsym(priv->h_egl, procname);
    if (p)
        return p;

    if (priv->eglGetProcAddress)
        p = priv->eglGetProcAddress(procname);

    if (!p)
        WINE_ERR("%s is missing but required\n", procname);

    return p;
}

static BOOL dri2_create(Display *dpy, int screen, struct dri_backend_priv **priv)
{
    struct dri2_priv *p;
    char *device;
    int fd;
    Window root = RootWindow(dpy, screen);
    drm_auth_t auth;

    if (!dri2_connect(dpy, root, DRI2DriverDRI, &device))
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

    if (!dri2_authenticate(dpy, root, auth.magic))
    {
        close(fd);
        return FALSE;
    }

    p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct dri2_priv));
    if (!p)
    {
        close(fd);
        return FALSE;
    }

    p->dpy = dpy;
    p->screen = screen;
    p->fd = fd;

    p->h_egl = dlopen(lib_egl, RTLD_LAZY);
    if (!p->h_egl)
    {
        WINE_ERR("failed to open %s: %s\n", lib_egl, dlerror());
        goto err_egl;
    }

#define DRI2_EGLGETPROCADDRESS(procname) \
    p->procname = dri2_eglGetProcAddress(p, #procname); \
    if (!p->procname) \
        goto err_egl;

    DRI2_EGLGETPROCADDRESS(eglGetProcAddress);
    DRI2_EGLGETPROCADDRESS(eglCreateContext);
    DRI2_EGLGETPROCADDRESS(eglDestroyContext);
    DRI2_EGLGETPROCADDRESS(eglGetError);
    DRI2_EGLGETPROCADDRESS(eglInitialize);
    DRI2_EGLGETPROCADDRESS(eglMakeCurrent);
    DRI2_EGLGETPROCADDRESS(eglQueryString);
    DRI2_EGLGETPROCADDRESS(eglTerminate);
    DRI2_EGLGETPROCADDRESS(eglChooseConfig);
    DRI2_EGLGETPROCADDRESS(eglBindAPI);
    DRI2_EGLGETPROCADDRESS(eglQueryAPI);
    DRI2_EGLGETPROCADDRESS(eglCreateImageKHR);
    DRI2_EGLGETPROCADDRESS(eglDestroyImageKHR);
    DRI2_EGLGETPROCADDRESS(eglGetPlatformDisplayEXT);

    DRI2_EGLGETPROCADDRESS(glFlush);
    DRI2_EGLGETPROCADDRESS(glTexParameteri);
    DRI2_EGLGETPROCADDRESS(glGenTextures);
    DRI2_EGLGETPROCADDRESS(glDeleteTextures);
    DRI2_EGLGETPROCADDRESS(glBindTexture);
    DRI2_EGLGETPROCADDRESS(glEGLImageTargetTexture2DOES);
    DRI2_EGLGETPROCADDRESS(glBindFramebuffer);
    DRI2_EGLGETPROCADDRESS(glDeleteFramebuffers);
    DRI2_EGLGETPROCADDRESS(glGenFramebuffers);
    DRI2_EGLGETPROCADDRESS(glCheckFramebufferStatus);
    DRI2_EGLGETPROCADDRESS(glFramebufferTexture2D);
    DRI2_EGLGETPROCADDRESS(glBlitFramebuffer);

#undef DRI2_EGLGETPROCADDRESS

    *priv = (struct dri_backend_priv *)p;

    return TRUE;

err_egl:
    close(fd);
    HeapFree(GetProcessHeap(), 0, p);
    return FALSE;
}

static BOOL dri2_init(struct dri_backend_priv *priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
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

    current_api = p->eglQueryAPI();

    if (!display)
        display = p->eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, p->dpy, NULL);
    if (!display)
        return FALSE;
    /* count references on display for multi device setups */
    display_ref++;

    if (p->eglInitialize(display, &major, &minor) != EGL_TRUE)
        goto clean_egl_display;

    extensions = p->eglQueryString(display, EGL_CLIENT_APIS);
    if (!extensions || !strstr(extensions, "OpenGL"))
        goto clean_egl_display;

    extensions = p->eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
            !strstr(extensions, "EGL_KHR_create_context") ||
            !strstr(extensions, "EGL_KHR_surfaceless_context") ||
            !strstr(extensions, "EGL_KHR_image_base"))
        goto clean_egl_display;

    if (!p->eglChooseConfig(display, config_attribs, &config, 1, &i))
        goto clean_egl_display;

    b = p->eglBindAPI(EGL_OPENGL_API);
    if (b == EGL_FALSE)
        goto clean_egl_display;
    context = p->eglCreateContext(display, config, EGL_NO_CONTEXT, context_compatibility_attribs);
    if (context == EGL_NO_CONTEXT)
        goto clean_egl_display;

    p->eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    p->display = display;
    p->context = context;

    p->eglBindAPI(current_api);
    return TRUE;

clean_egl_display:
    p->eglTerminate(display);
    p->eglBindAPI(current_api);
    return FALSE;
}

static int dri2_get_fd(struct dri_backend_priv *priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;

    return p->fd;
}

static BOOL dri2_present_pixmap(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
    struct dri2_pixmap_priv *pp = (struct dri2_pixmap_priv *)buffer_priv;
    EGLenum current_api = 0;

    current_api = p->eglQueryAPI();
    p->eglBindAPI(EGL_OPENGL_API);
    if (p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context))
    {
        p->glBindFramebuffer(GL_READ_FRAMEBUFFER, pp->fbo_read);
        p->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pp->fbo_write);

        p->glBlitFramebuffer(0, 0, pp->width, pp->height, 0, 0, pp->width, pp->height,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p->glFlush(); /* Perhaps useless */
    }
    else
    {
        WINE_ERR("eglMakeCurrent failed with 0x%0X\n", p->eglGetError());
        return FALSE;
    }

    p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p->eglBindAPI(current_api);

    return TRUE;
}

static BOOL dri2_present(struct dri_backend_priv *priv, int fd, int width, int height, int stride,
        int depth, int bpp, struct buffer_priv **buffer_priv, Pixmap *pixmap)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
    struct dri2_pixmap_priv *pp;
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

    WINE_TRACE("fd=%d, width=%d, height=%d, stride=%d, depth=%d, bpp=%d\n",
            fd, width, height, stride, depth, bpp);

    attribs[1] = width;
    attribs[3] = height;
    attribs[7] = fd;
    attribs[11] = stride;

    current_api = p->eglQueryAPI();
    p->eglBindAPI(EGL_OPENGL_API);

    /* We bind the dma-buf to a EGLImage, then to a texture, and then to a fbo.
     * Note that we can delete the EGLImage, but we shouldn't delete the texture,
     * else the fbo is invalid */

    image = p->eglCreateImageKHR(p->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                 NULL, attribs);

    if (image == EGL_NO_IMAGE_KHR) {
        WINE_ERR("eglCreateImageKHR failed with 0x%0X\n", p->eglGetError());
        goto fail;
    }
    close(fd);

    if (p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context))
    {
        p->glGenTextures(1, &texture_read);
        p->glBindTexture(GL_TEXTURE_2D, texture_read);
        p->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        p->glGenFramebuffers(1, &fbo_read);
        p->glBindFramebuffer(GL_FRAMEBUFFER, fbo_read);
        p->glFramebufferTexture2D(GL_FRAMEBUFFER,
                                  GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, texture_read,
                                  0);
        status = p->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            goto fail;
        p->glBindTexture(GL_TEXTURE_2D, 0);
        p->eglDestroyImageKHR(p->display, image);

        /* We bind a newly created pixmap (to which we want to copy the content)
         * to an EGLImage, then to a texture, then to a fbo. */
        image = p->eglCreateImageKHR(p->display, p->context, EGL_NATIVE_PIXMAP_KHR,
                                     (void *)*pixmap, NULL);
        if (image == EGL_NO_IMAGE_KHR)
            goto fail;

        p->glGenTextures(1, &texture_write);
        p->glBindTexture(GL_TEXTURE_2D, texture_write);
        p->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        p->glGenFramebuffers(1, &fbo_write);
        p->glBindFramebuffer(GL_FRAMEBUFFER, fbo_write);
        p->glFramebufferTexture2D(GL_FRAMEBUFFER,
                                  GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, texture_write,
                                  0);
        status = p->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            goto fail;
        p->glBindTexture(GL_TEXTURE_2D, 0);
        p->eglDestroyImageKHR(p->display, image);
    }
    else
        WINE_ERR("eglMakeCurrent failed with 0x%0X\n", p->eglGetError());

    p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    pp = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct dri2_pixmap_priv));

    if (!pp)
        goto fail;

    pp->fbo_read = fbo_read;
    pp->fbo_write = fbo_write;
    pp->texture_read = texture_read;
    pp->texture_write = texture_write;
    pp->width = width;
    pp->height = height;
    pp->next = p->first_dri2_priv;
    p->first_dri2_priv = pp;

    *buffer_priv = (struct buffer_priv *)pp;

    p->eglBindAPI(current_api);

    return TRUE;
fail:
    p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p->eglBindAPI(current_api);
    return FALSE;
}

static BOOL dri2_window_buffer_from_dmabuf(struct dri_backend_priv *priv,
    PRESENTpriv *present_priv, int fd, int width, int height,
    int stride, int depth, int bpp, struct D3DWindowBuffer **out)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
    Pixmap pixmap;

    WINE_TRACE("present_priv=%p dmaBufFd=%d\n", present_priv, fd);

    if (!out)
        return FALSE;

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct D3DWindowBuffer));
    if (!*out)
        return FALSE;

    if (!PRESENTPixmapCreate(present_priv, p->screen, &pixmap,
            width, height, stride, depth, bpp))
    {
        HeapFree(GetProcessHeap(), 0, *out);
        WINE_ERR("Failed to create pixmap\n");
        return FALSE;
    }

    if (!dri2_present(priv, fd, width, height, stride, depth, bpp,
            &(*out)->priv, &pixmap))
    {
        WINE_ERR("dri2_present failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    if (!PRESENTPixmapInit(present_priv, pixmap, &((*out)->present_pixmap_priv)))
    {
        WINE_ERR("PRESENTPixmapInit failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    return TRUE;
}

static BOOL dri2_copy_front(PRESENTPixmapPriv *present_pixmap_priv)
{
    return FALSE;
}

static void dri2_destroy_pixmap(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
    struct dri2_pixmap_priv *pp = (struct dri2_pixmap_priv *)buffer_priv;
    EGLenum current_api;

    if (p->first_dri2_priv == pp)
    {
        p->first_dri2_priv = pp->next;
    }
    else
    {
        struct dri2_pixmap_priv *current;

        current = p->first_dri2_priv;
        while (current->next != pp)
            current = current->next;
        current->next = pp->next;
    }

    current_api = p->eglQueryAPI();

    p->eglBindAPI(EGL_OPENGL_API);
    if (p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context))
    {
        p->glDeleteFramebuffers(1, &pp->fbo_read);
        p->glDeleteFramebuffers(1, &pp->fbo_write);
        p->glDeleteTextures(1, &pp->texture_read);
        p->glDeleteTextures(1, &pp->texture_write);
    }
    else
        WINE_ERR("eglMakeCurrent failed with 0x%0X\n", p->eglGetError());

    p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p->eglBindAPI(current_api);

    HeapFree(GetProcessHeap(), 0, pp);
}

/* hypothesis: at this step all textures, etc are destroyed */
static void dri2_deinit(struct dri_backend_priv *priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;
    EGLenum current_api;
    struct dri2_pixmap_priv *current;

    current = p->first_dri2_priv;
    while (current)
    {
        struct dri2_pixmap_priv *next = current->next;
        dri2_destroy_pixmap(priv, (struct buffer_priv *)current);
        current = next;
    }

    current_api = p->eglQueryAPI();
    p->eglBindAPI(EGL_OPENGL_API);
    p->eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p->eglDestroyContext(p->display, p->context);
    if (display)
    {
        /* destroy display connection with last device */
        display_ref--;
        if (!display_ref)
        {
            p->eglTerminate(display);
            display = NULL;
        }
    }
    p->eglBindAPI(current_api);
}

static void dri2_destroy(struct dri_backend_priv *priv)
{
    struct dri2_priv *p = (struct dri2_priv *)priv;

    if (!display_ref)
        dlclose(p->h_egl);

    close(p->fd);

    HeapFree(GetProcessHeap(), 0, p);
}

static BOOL dri2_probe(Display *dpy)
{
    struct dri_backend_priv *priv;
    BOOL res;

    xcb_connection_t *conn = XGetXCBConnection(dpy);
    xcb_dri2_query_version_cookie_t dri2_cookie;
    xcb_dri2_query_version_reply_t *dri2_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;
    /* Request API version 1.4 */
    const int major = 1;
    const int minor = 4;

    xcb_prefetch_extension_data(conn, &xcb_dri2_id);

    extension = xcb_get_extension_data(conn, &xcb_dri2_id);
    if (!(extension && extension->present))
    {
        WINE_ERR("DRI2 extension is not present\n");
        return FALSE;
    }

    dri2_cookie = xcb_dri2_query_version(conn, major, minor);

    dri2_reply = xcb_dri2_query_version_reply(conn, dri2_cookie, &error);
    if (!dri2_reply)
    {
        free(error);
        WINE_ERR("Issue getting requested v%d.%d of DRI2\n", major, minor);
        return FALSE;
    }

    WINE_TRACE("DRI2 v%d.%d requested, v%d.%d found\n", major, minor,
            (int)dri2_reply->major_version, (int)dri2_reply->minor_version);
    free(dri2_reply);

    if (!dri2_create(dpy, DefaultScreen(dpy), &priv))
        return FALSE;

    res = dri2_init(priv);

    dri2_destroy(priv);

    return res;
}

const struct dri_backend_funcs dri2_funcs = {
    .name = "dri2",
    .probe = dri2_probe,
    .create = dri2_create,
    .destroy = dri2_destroy,
    .init = dri2_init,
    .deinit = dri2_deinit,
    .get_fd = dri2_get_fd,
    .window_buffer_from_dmabuf = dri2_window_buffer_from_dmabuf,
    .copy_front = dri2_copy_front,
    .present_pixmap = dri2_present_pixmap,
    .destroy_pixmap = dri2_destroy_pixmap,
};
#endif
