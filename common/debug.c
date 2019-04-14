/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <assert.h>

#include "debug.h"

/* a single simple ring buffer for all threads */
#define NINE_DEBUG_BUFFERSIZE 1024

static struct
{
    LONG pos;
    char buf[NINE_DEBUG_BUFFERSIZE];
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

    pos_new = (pos_use + len);

    if (!__sync_bool_compare_and_swap(&__nine_debug.pos, pos_cur, pos_new))
        goto retry;

    return __nine_debug.buf + pos_use;
}

const char *__nine_dbg_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *buf = __nine_debug_buf(n);

    if (!buf)
        return NULL;

    return memcpy(buf, s, n);
}

