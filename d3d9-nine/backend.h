/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9 DRI backend interface
 *
 * Copyright 2019 Patrick Rudolph
 */

#ifndef __NINE_BACKEND_H
#define __NINE_BACKEND_H

#include <X11/Xlib.h>

struct dri_backend_priv;
struct buffer_priv;
struct PRESENTpriv;
struct PRESENTPixmapPriv;
typedef struct PRESENTPriv PRESENTpriv;
typedef struct PRESENTPixmapPriv PRESENTPixmapPriv;

struct D3DWindowBuffer
{
    PRESENTPixmapPriv *present_pixmap_priv;
    struct buffer_priv *priv; /* backend private data */
};

struct dri_backend_funcs {
    const char * const name;

    BOOL (*probe)(Display *dpy);

    BOOL (*create)(Display *dpy, int screen, struct dri_backend_priv **priv);
    void (*destroy)(struct dri_backend_priv *priv);

    BOOL (*init)(struct dri_backend_priv *priv);
    void (*deinit)(struct dri_backend_priv *priv);
    int (*get_fd)(struct dri_backend_priv *priv);

    BOOL (*window_buffer_from_dmabuf)(struct dri_backend_priv *priv,
        PRESENTpriv *present_priv, int fd, int width, int height,
        int stride, int depth, int bpp, struct D3DWindowBuffer **out);
    BOOL (*copy_front)(PRESENTPixmapPriv *present_pixmap_priv);

    BOOL (*present_pixmap)(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv);
    void (*destroy_pixmap)(struct dri_backend_priv *priv, struct buffer_priv *buffer_priv);
};

struct dri_backend {
    const struct dri_backend_funcs *funcs;
    struct dri_backend_priv *priv; /* backend private data */
};

BOOL backend_probe(Display *dpy);

struct dri_backend *backend_create(Display *dpy, int screen);
void backend_destroy(struct dri_backend *dri_backend);

#endif /* __NINE_BACKEND_H */
