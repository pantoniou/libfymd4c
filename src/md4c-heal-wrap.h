/*
 * MD4C: Markdown parser for C
 * (http://github.com/unjs/md4c)
 *
 * Shared heal-before-render wrapper (header-only).
 * Include this in renderer .c files that support MD_*_FLAG_HEAL.
 */

#ifndef MD4C_HEAL_WRAP_H
#define MD4C_HEAL_WRAP_H

#include <stdlib.h>
#include <string.h>
#include "md4c-heal.h"

/* Common heal flag value — all renderers use 0x0100. */
#define MD4C_FLAG_HEAL 0x0100

typedef struct
{
    char *data;
    unsigned size;
    unsigned cap;
    int error;
} MD4C_HEAL_BUF;

static void
md4c_heal_buf_append(const char *text, unsigned size, void *userdata)
{
    MD4C_HEAL_BUF *buf = (MD4C_HEAL_BUF *)userdata;
    if (buf->error) return;
    if (buf->size + size > buf->cap)
    {
        unsigned new_cap = buf->cap + buf->cap / 2 + size + 256;
        char *p = (char *)realloc(buf->data, new_cap);
        if (!p) { buf->error = 1; return; }
        buf->data = p;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->size, text, size);
    buf->size += size;
}

/* Run md_heal and return the healed buffer. Caller must free buf->data.
 * Returns 0 on success, -1 on error. */
static int
md4c_heal_input(const MD_CHAR *input, MD_SIZE input_size, MD4C_HEAL_BUF *buf)
{
    int ret;
    buf->data = NULL;
    buf->size = 0;
    buf->cap = 0;
    buf->error = 0;
    ret = md_heal(input, input_size, md4c_heal_buf_append, buf);
    if(buf->error) return -1;
    return ret;
}

#endif /* MD4C_HEAL_WRAP_H */
