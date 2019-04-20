/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "debug.h"

unsigned char __nine_debug_flags = (1 << __NINE_DBCL_FIXME) |
                                   (1 << __NINE_DBCL_ERR);

/* a single simple ring buffer for all threads */
#define NINE_DEBUG_BUFFERSIZE 1024

static struct
{
    char buf[NINE_DEBUG_BUFFERSIZE] NINE_ATTR_ALIGNED(16);
    LONG pos;
} __nine_debug;

static char *__nine_debug_buf(size_t len)
{
    LONG pos_cur, pos_use, pos_new;

    if (len > NINE_DEBUG_BUFFERSIZE)
        return NULL;

retry:
    pos_cur = __nine_debug.pos;

    if (pos_cur + len > NINE_DEBUG_BUFFERSIZE)
        pos_use = 0;
    else
        pos_use = pos_cur;

    pos_new = (pos_use + len + 15) & ~15;

    if (!__sync_bool_compare_and_swap(&__nine_debug.pos, pos_cur, pos_new))
        goto retry;

    return __nine_debug.buf + pos_use;
}

const char *__nine_dbg_strdup(const char *s, size_t len)
{
    char *buf = __nine_debug_buf(len + 1);

    if (!buf)
        return NULL;

    return memcpy(buf, s, len + 1);
}

static void nine_dbg_init() __attribute__((constructor));
static void nine_dbg_init()
{
    char *env;
    struct stat st1, st2;

    /* check for stderr pointing to /dev/null */
    if (!fstat(STDERR_FILENO, &st1) && S_ISCHR(st1.st_mode) &&
        !stat("/dev/null", &st2) && S_ISCHR(st2.st_mode) &&
        st1.st_rdev == st2.st_rdev)
    {
        __nine_debug_flags = 0;
        return;
    }

    /* new style debug mask */
    env = getenv("D3D_DEBUG");
    if (env)
    {
        __nine_debug_flags = strtol(env, NULL, 0);
        return;
    }

    /* fallback to old style WINE debug channel */
    env = getenv("WINEDEBUG");
    if (!env)
        return;

    /* just the most basic version, no support for classes */
    if (strstr(env, "d3d9nine"))
        __nine_debug_flags = (1 << __NINE_DBCL_FIXME) |
                             (1 << __NINE_DBCL_ERR) |
                             (1 << __NINE_DBCL_WARN) |
                             (1 << __NINE_DBCL_TRACE);
}
