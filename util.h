/* berrycode - small shared helpers (growable string buffer, misc) */
#ifndef BC_UTIL_H
#define BC_UTIL_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct {
    char  *p;      /* always NUL-terminated once anything was added */
    size_t n;      /* length (excluding NUL) */
    size_t cap;
} sbuf;

void  sb_add(sbuf *b, const char *s, size_t n);
void  sb_adds(sbuf *b, const char *s);
void  sb_addc(sbuf *b, char c);
void  sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  sb_vprintf(sbuf *b, const char *fmt, va_list ap);
char *sb_take(sbuf *b);            /* returns malloc'd string ("" if empty), resets b */
void  sb_free(sbuf *b);
const char *sb_str(const sbuf *b); /* "" if empty */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* read a whole file; returns malloc'd buffer (NUL-terminated) or NULL; *len optional */
char *read_file_all(const char *path, size_t *len);
int   write_file_all(const char *path, const char *data, size_t len);
int   mkdir_p(const char *dir);        /* 0 ok */
int   mkdir_parents(const char *path); /* make parent dirs of a file path */

/* read one line from fp (without trailing newline). NULL on EOF/error with nothing read.
 * *interrupted set if read was interrupted by a signal (EINTR). */
char *read_line(FILE *fp, int *interrupted);

/* Display columns (Q20/Term49CX rules): 0 for controls/combining marks,
 * 2 for East-Asian wide/fullwidth + the 1F000-1FAFF emoji block Term49CX
 * treats as wide, 1 otherwise. Matches ecma48_char_width() in Term49CX
 * (ICU EA_WIDE/FULLWIDTH) without linking ICU. */
unsigned utf8_decode(const char *s, size_t n, size_t *len);
int      uc_width(unsigned cp);
int      utf8_cols(const char *s, size_t n);        /* display columns in s */
/* bytes of s fitting in maxcols display columns (never splits a char) */
size_t   utf8_take_cols(const char *s, size_t n, int maxcols, int *truncated);

/* Model-output filter: passes printable text (UTF-8, \n, \t) to both
 * buffers, preserves safe SGR colour sequences (ESC [ ... m) in *disp
 * only, and drops everything else (cursor moves, clears, OSC 52
 * clipboard, alt-screen/mouse/paste-mode switches, \r, controls).
 * Stateful so sequences split across stream chunks are handled. */
typedef struct { int st; char seq[64]; int n; } mfilter;
void mf_init(mfilter *f);
void mf_add(mfilter *f, const char *s, size_t n, sbuf *plain, sbuf *disp);
void mf_flush(mfilter *f);

/* Terminal-safe text: drops ANSI escape sequences, \r, backspaces and other control
 * characters (keeps \n and \t), so tool output can't move the cursor and scribble
 * over earlier lines. Stateful so sequences split across chunks are handled. */
typedef struct { int st; } tfilter;
int   tf_char(tfilter *f, unsigned char c);      /* 1 = print c, 0 = drop it */
char *term_clean(const char *s, size_t n);       /* malloc'd cleaned copy */
int   utf8_cols(const char *s, size_t n);        /* characters (≈ columns) in s */

#endif
