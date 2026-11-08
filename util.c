#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "berrycode: out of memory\n"); exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "berrycode: out of memory\n"); exit(2); }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *xstrdup(const char *s) { return xstrndup(s ? s : "", s ? strlen(s) : 0); }

static void sb_grow(sbuf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->n + extra + 1) nc *= 2;
    b->p = xrealloc(b->p, nc);
    b->cap = nc;
}

void sb_add(sbuf *b, const char *s, size_t n)
{
    sb_grow(b, n);
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void sb_adds(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }
void sb_addc(sbuf *b, char c) { sb_add(b, &c, 1); }

void sb_vprintf(sbuf *b, const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    char probe[1];
    int k = vsnprintf(probe, sizeof probe, fmt, cp);
    va_end(cp);
    if (k < 0) return;
    sb_grow(b, (size_t)k);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    b->n += (size_t)k;
}

void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

char *sb_take(sbuf *b)
{
    char *r = b->p ? b->p : xstrdup("");
    b->p = NULL; b->n = b->cap = 0;
    return r;
}

void sb_free(sbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
const char *sb_str(const sbuf *b) { return b->p ? b->p : ""; }

char *read_file_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    sbuf b = {0};
    char tmp[8192];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) sb_add(&b, tmp, k);
    int err = ferror(f);
    fclose(f);
    if (err) { sb_free(&b); return NULL; }
    if (len) *len = b.n;
    return sb_take(&b);
}

int write_file_all(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int rc = (w == len) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

int mkdir_p(const char *dir)
{
    char *d = xstrdup(dir);
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/') d[--n] = 0;
    for (char *p = d + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(d, 0755) != 0 && errno != EEXIST) { free(d); return -1; }
        *p = '/';
    }
    int rc = (mkdir(d, 0755) == 0 || errno == EEXIST) ? 0 : -1;
    free(d);
    return rc;
}

int mkdir_parents(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    char *d = xstrndup(path, (size_t)(slash - path));
    int rc = mkdir_p(d);
    free(d);
    return rc;
}

int tf_char(tfilter *f, unsigned char c)
{
    switch (f->st) {
    case 0:
        if (c == 0x1b) { f->st = 1; return 0; }
        if (c == '\n' || c == '\t') return 1;
        return (c >= 0x20 && c != 0x7f) ? 1 : 0;
    case 1:                                      /* after ESC */
        if (c == '[') f->st = 2;
        else if (c == ']' || c == 'P' || c == '_' || c == '^') f->st = 3;
        else f->st = 0;
        return 0;
    case 2:                                      /* CSI ... final byte */
        if (c >= 0x40 && c <= 0x7e) f->st = 0;
        return 0;
    case 3:                                      /* OSC/DCS string until BEL or ESC \ */
        if (c == 7) f->st = 0;
        else if (c == 0x1b) f->st = 4;
        return 0;
    default:
        f->st = (c == '\\') ? 0 : 3;
        return 0;
    }
}

char *term_clean(const char *s, size_t n)
{
    tfilter f = {0};
    sbuf b = {0};
    sb_add(&b, "", 0);
    for (size_t i = 0; i < n; i++)
        if (tf_char(&f, (unsigned char)s[i])) sb_addc(&b, s[i] == '\t' ? ' ' : s[i]);
    return sb_take(&b);
}

unsigned utf8_decode(const char *s, size_t n, size_t *len)
{
    if (!n) { if (len) *len = 0; return 0; }
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { if (len) *len = 1; return c0; }
    size_t want = 0;
    unsigned cp = 0;
    if ((c0 & 0xE0) == 0xC0) { want = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { want = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { want = 4; cp = c0 & 0x07; }
    else { if (len) *len = 1; return 0xFFFD; }   /* stray continuation */
    if (want > n) { if (len) *len = 1; return 0xFFFD; }  /* truncated: one bad byte */
    for (size_t i = 1; i < want; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) { if (len) *len = 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (len) *len = want;
    /* reject overlongs / surrogates / out-of-range as replacement */
    if ((want == 2 && cp < 0x80) || (want == 3 && cp < 0x800) ||
        (want == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

int uc_width(unsigned cp)
{
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    /* combining marks: zero width */
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) return 0;
    /* wide/fullwidth: match Term49CX (ICU EA_WIDE/FULLWIDTH) + its
     * explicit 1F000-1FAFF emoji-wide rule. Ambiguous emoji/symbols
     * bias to 2: overcounting only shortens a line (safe), while
     * undercounting would push into the last column and break the
     * pending-wrap invariant berrycode and Term49CX both rely on. */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x20000 && cp <= 0x3FFFD) ||
        (cp >= 0x1F000 && cp <= 0x1FAFF) ||
        (cp >= 0x1F300 && cp <= 0x1F6FF) ||
        (cp >= 0x2600 && cp <= 0x27BF)) return 2;
    return 1;
}

int utf8_cols(const char *s, size_t n)
{
    int c = 0;
    size_t i = 0;
    while (i < n) {
        size_t len = 0;
        unsigned cp = utf8_decode(s + i, n - i, &len);
        if (!len) break;
        i += len;
        c += uc_width(cp);
    }
    return c;
}

/* take up to maxcols display columns from s (never split a char);
 * returns bytes consumed, sets *truncated if input remains. */
size_t utf8_take_cols(const char *s, size_t n, int maxcols, int *truncated)
{
    size_t i = 0;
    int cols = 0;
    while (i < n) {
        size_t len = 0;
        unsigned cp = utf8_decode(s + i, n - i, &len);
        if (!len) break;
        int w = uc_width(cp);
        if (cols + w > maxcols) break;   /* wide char doesn't fit: wrap before it */
        cols += w;
        i += len;
    }
    if (truncated) *truncated = (i < n);
    return i;
}

extern volatile sig_atomic_t g_winch_soft;   /* set by the SIGWINCH handler in main.c */

char *read_line(FILE *fp, int *interrupted)
{
    sbuf b = {0};
    if (interrupted) *interrupted = 0;
    for (;;) {
        errno = 0;
        int c = fgetc(fp);
        if (c == EOF) {
            if (ferror(fp) && errno == EINTR) {
                clearerr(fp);
                /* a window resize interrupts the read too; that is not
                 * Ctrl-C, so just retry instead of reporting interrupt */
                if (g_winch_soft) { g_winch_soft = 0; continue; }
                if (interrupted) *interrupted = 1;
                sb_free(&b);
                return NULL;
            }
            if (b.n == 0 && !b.p) return NULL;
            break;
        }
        if (c == '\n') break;
        sb_addc(&b, (char)c);
    }
    char *s = sb_take(&b);
    size_t n = strlen(s);
    if (n && s[n - 1] == '\r') s[n - 1] = 0;
    return s;
}

/* SGR allowlist check: only numeric params (digits + ';'), short enough
 * to be a real rendition sequence. Everything SGR can do is change
 * colours/attributes, so it is safe to pass through to Term49CX. */
static int sgr_ok(const char *seq, int n)
{
    if (n > 48) return 0;
    if (!n) return 1;                       /* plain ESC[m == reset */
    int digits = 0;
    for (int i = 0; i < n; i++) {
        if (seq[i] >= '0' && seq[i] <= '9') digits++;
        else if (seq[i] == ';') { if (!digits) return 0; digits = 0; }
        else return 0;
    }
    return digits > 0;
}

void mf_init(mfilter *f) { f->st = 0; f->n = 0; }

void mf_add(mfilter *f, const char *s, size_t n, sbuf *plain, sbuf *disp)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (f->st) {
        case 0:                                     /* normal text */
            if (c == 0x1b) { f->st = 1; f->n = 0; }
            else if (c == '\n' || c == '\t') { sb_addc(plain, (char)c); sb_addc(disp, (char)c); }
            else if (c == '\r') { /* drop: models emit \r\n, Term49CX wants \n */ }
            else if (c < 0x20 || c == 0x7f) { /* drop other controls */ }
            else { sb_addc(plain, (char)c); sb_addc(disp, (char)c); }
            break;
        case 1:                                     /* after ESC */
            if (c == '[') { f->st = 2; f->n = 0; }
            else if (c == ']' || c == 'P' || c == '_' || c == '^') f->st = 3;
            else if (c == '(' || c == ')' || c == '#' || c == '%') f->st = 5;
            else f->st = 0;                         /* single-char sequence: drop */
            break;
        case 2:                                     /* CSI params */
            if ((c >= '0' && c <= '9') || c == ';' || (c >= ' ' && c <= '/') || c == '?') {
                if (f->n < (int)sizeof f->seq) f->seq[f->n++] = (char)c;
            } else if (c >= '@' && c <= '~') {
                if (c == 'm' && sgr_ok(f->seq, f->n)) {
                    sb_addc(disp, 0x1b); sb_addc(disp, '[');
                    sb_add(disp, f->seq, (size_t)f->n);
                    sb_addc(disp, 'm');
                }
                f->st = 0;                          /* anything else CSI: drop */
            } else {
                f->st = 0;
            }
            break;
        case 3:                                     /* OSC/DCS string */
            if (c == 7) f->st = 0;                  /* BEL ends it */
            else if (c == 0x1b) f->st = 4;
            break;
        case 4:                                     /* ESC inside string */
            f->st = (c == '\\') ? 0 : 3;
            break;
        default:                                    /* charset second byte */
            f->st = 0;
            break;
        }
    }
}

void mf_flush(mfilter *f) { f->st = 0; f->n = 0; }  /* drop any partial sequence */
