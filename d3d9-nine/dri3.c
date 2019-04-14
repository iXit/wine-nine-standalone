/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine DRI3 interface
 *
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015 Patrick Rudolph
 */

#include <windows.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "../common/debug.h"
#include "backend.h"
#include "xcb_present.h"

struct dri3_priv {
    Display *dpy;
    int screen;
    int fd;
};

static BOOL dri3_create(Display *dpy, int screen, struct dri_backend_priv **priv)
{
    struct dri3_priv *p;
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

    free(reply);

    p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct dri3_priv));
    if (!p)
    {
        close(fd);
        return FALSE;
    }

    p->dpy = dpy;
    p->screen = screen;
    p->fd = fd;

    *priv = (struct dri_backend_priv *)p;

    return TRUE;
}

static void dri3_destroy(struct dri_backend_priv *priv)
{
    struct dri3_priv *p = (struct dri3_priv *)priv;

    close(p->fd);

    HeapFree(GetProcessHeap(), 0, p);
}

static BOOL dri3_init(struct dri_backend_priv *priv)
{
    return TRUE;
}

static void dri3_deinit(struct dri_backend_priv *priv)
{
}

static int dri3_get_fd(struct dri_backend_priv *priv)
{
    struct dri3_priv *p = (struct dri3_priv *)priv;

    return p->fd;
}

static BOOL dri3_window_buffer_from_dmabuf(struct dri_backend_priv *priv,
    PRESENTpriv *present_priv, int fd, int width, int height,
    int stride, int depth, int bpp, struct D3DWindowBuffer **out)
{
    struct dri3_priv *p = (struct dri3_priv *)priv;
    Pixmap pixmap;
    xcb_connection_t *xcb_connection = XGetXCBConnection(p->dpy);
    Window root = RootWindow(p->dpy, p->screen);
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;

    TRACE("present_priv=%p dmaBufFd=%d\n", present_priv, fd);

    if (!out)
        goto err;

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct D3DWindowBuffer));
    if (!*out)
        goto err;

    cookie = xcb_dri3_pixmap_from_buffer_checked(xcb_connection,
            (pixmap = xcb_generate_id(xcb_connection)), root, 0,
            width, height, stride, depth, bpp, fd);

    error = xcb_request_check(xcb_connection, cookie); /* performs a flush */
    if (error)
    {
        ERR("Error using DRI3 to convert a DmaBufFd to pixmap\n");
        goto err;
    }

    if (!PRESENTPixmapInit(present_priv, pixmap, &((*out)->present_pixmap_priv)))
    {
        ERR("PRESENTPixmapInit failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    return TRUE;

err:
    ERR("dri3_window_buffer_from_dmabuf failed\n");
    if (out)
        HeapFree(GetProcessHeap(), 0, *out);
    return FALSE;
}

static BOOL dri3_copy_front(PRESENTPixmapPriv *present_pixmap_priv)
{
    return PRESENTHelperCopyFront(present_pixmap_priv);
}

static BOOL dri3_present_pixmap(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv)
{
    return TRUE;
}

static void dri3_destroy_pixmap(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv)
{
}

static BOOL dri3_probe(Display *dpy)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_dri3_query_version_cookie_t dri3_cookie;
    xcb_dri3_query_version_reply_t *dri3_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;
    const int major = 1;
    const int minor = 0;

    xcb_prefetch_extension_data(xcb_connection, &xcb_dri3_id);

    extension = xcb_get_extension_data(xcb_connection, &xcb_dri3_id);
    if (!(extension && extension->present))
    {
        WARN("DRI3 extension is not present\n");
        return FALSE;
    }

    dri3_cookie = xcb_dri3_query_version(xcb_connection, major, minor);

    dri3_reply = xcb_dri3_query_version_reply(xcb_connection, dri3_cookie, &error);
    if (!dri3_reply)
    {
        free(error);
        WARN("Issue getting requested v%d.%d of DRI3\n", major, minor);
        return FALSE;
    }

    TRACE("DRI3 v%d.%d found, v%d.%d requested\n", major, minor,
          (int)dri3_reply->major_version, (int)dri3_reply->minor_version);
    free(dri3_reply);

    return TRUE;
}

const struct dri_backend_funcs dri3_funcs = {
    .name = "dri3",
    .probe = dri3_probe,
    .create = dri3_create,
    .destroy = dri3_destroy,
    .init = dri3_init,
    .deinit = dri3_deinit,
    .get_fd = dri3_get_fd,
    .window_buffer_from_dmabuf = dri3_window_buffer_from_dmabuf,
    .copy_front = dri3_copy_front,
    .present_pixmap = dri3_present_pixmap,
    .destroy_pixmap = dri3_destroy_pixmap,
};
