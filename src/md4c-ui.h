/*
 * md4c-ui.h - UI Markdown: fy-* tags, layout markers and clickable regions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MD4C_UI_H
#define MD4C_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A layout marker is a private APC string, "ESC _ fy: kind [=arg] ESC \". It
 * has no display width, so line wrapping and table layout pass it through, and
 * the renderer resolves or removes it before the output leaves the library.
 */
#define MD_UI_MARK_OPEN     "\x1b_fy:"
#define MD_UI_MARK_OPEN_LEN 5
#define MD_UI_MARK_CLOSE    "\x1b\\"

typedef enum {
    MD_UI_REGION_ACT,           /* the label of an fy-act, one row */
    MD_UI_REGION_SLOT           /* an fy-slot that another component draws */
} MD_UI_REGION_KIND;

/* A region of the output. The id is owned. */
typedef struct MD_ANSI_REGION {
    char* id;
    size_t row;
    int col;
    int width;
    int height;
    int kind;                   /* MD_UI_REGION_KIND */
} MD_ANSI_REGION;

typedef struct MD_ANSI_UI {
    MD_ANSI_REGION* regions;
    size_t count;
    size_t alloc;
} MD_ANSI_UI;

void md_ui_reset(MD_ANSI_UI* ui);
void md_ui_fini(MD_ANSI_UI* ui);
int md_ui_region_add(MD_ANSI_UI* ui, const char* id, size_t id_len,
                     size_t row, int col, int width, int height, int kind);

/* Split a slot marker argument "ID:A:B". Returns 0, or -1 when it is not one. */
int md_ui_slot_arg(const char* arg, size_t len, size_t* id_len, int* a, int* b);

#define MD_UI_ATTR_MAX 6

typedef struct {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
} MD_UI_ATTR;

/* A parsed fy-* tag. The pointers refer to the parsed text. */
typedef struct {
    const char* name;           /* after the "fy-" prefix */
    size_t name_len;
    int closing;                /* </fy-name> */
    int self_closing;           /* <fy-name/> */
    MD_UI_ATTR attrs[MD_UI_ATTR_MAX];
    int n_attrs;
    size_t len;                 /* bytes of the tag */
} MD_UI_TAG;

/* Parse the tag at @s. Returns 0 for an fy-* tag, -1 for anything else. */
int md_ui_tag_parse(const char* s, size_t n, MD_UI_TAG* t);
int md_ui_tag_is(const MD_UI_TAG* t, const char* name);
/* The value of attribute @name, or NULL. */
const char* md_ui_tag_attr(const MD_UI_TAG* t, const char* name, size_t* len);
/* An id holds letters, digits and "_-.:/", 1 to 63 bytes. */
int md_ui_id_valid(const char* s, size_t n);

/* The length of the marker at @s, or 0. @kind and @arg point into @s. */
size_t md_ui_marker(const char* s, size_t n, const char** kind, size_t* kind_len,
                    const char** arg, size_t* arg_len);
int md_ui_marker_is(const char* kind, size_t kind_len, const char* name);

/*
 * Lay out a rendered document in @height rows: give the rows left over to the
 * vfill markers, and take the rows that do not fit from the scroll bodies.
 * Removes the row markers and moves the region rows with their content; a
 * region on a removed row is dropped. *out is malloc()ed. Returns 0 or -1.
 */
int md_ui_vertical(const char* in, size_t len, int height, MD_ANSI_UI* ui,
                   char** out, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif
