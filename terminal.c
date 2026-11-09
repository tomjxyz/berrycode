/* Fixed footer WITHOUT a partial scrolling region. Term49 discards lines
 * scrolled out of a region. Instead, clear the footer and issue a real LF
 * at the physical bottom, then return to the new blank content row. The
 * terminal archives the outgoing top row normally; no transcript is lost
 * or duplicated, and footer text never enters scrollback.
 *
 * All UI writes (including the editor and spinner) share this manager and
 * mutex. Cursor bookkeeping uses display cells, not bytes, and retains UTF-8
 * and ANSI sequences split between streaming chunks. */
#include "bc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int active, rows, cols, row, col, dirty, sync_depth;
static long history_top;
static long tokens_in, tokens_out;
static char escape[96], unicode[4];
static size_t ne, nu, want;

static void raw(const char *s) { fputs(s, stdout); }
static void position(int r, int c) { fprintf(stdout, "\033[%d;%dH", r + 1, c + 1); }

static void token_count(char *text, size_t size, long count)
{
    if (count > 1000000) snprintf(text, size, "%.1fM", count / 1000000.0);
    else if (count > 1000) snprintf(text, size, "%.1fK", count / 1000.0);
    else snprintf(text, size, "%ld", count);
    char *zero = strstr(text, ".0");
    if (zero) memmove(zero, zero + 2, strlen(zero + 2) + 1);
}

static void paint(void)
{
    char in[32], out[32], suffix[80];
    token_count(in, sizeof in, tokens_in);
    token_count(out, sizeof out, tokens_out);
    snprintf(suffix, sizeof suffix, " | %s / %s ", in, out);
    /* Keep counts visible when a provider prefixes a long model name. */
    char *model = term_clean(g_cfg.model ? g_cfg.model : "?", g_cfg.model ? strlen(g_cfg.model) : 1);
    for (char *p = model; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    int avail = cols - (int)strlen(suffix) - 1;
    if (avail < 1) avail = 1;
    size_t length = strlen(model), take = length;
    int shortened = 0;
    if (utf8_cols(model, length) > avail)
        take = utf8_take_cols(model, length, avail - 1, &shortened);
    sbuf text = {0};
    sb_addc(&text, ' ');
    sb_add(&text, model, take);
    if (shortened) sb_adds(&text, "…");
    sb_adds(&text, suffix);
    free(model);
    const char *clean = sb_str(&text);
    int truncated = 0;
    size_t n = strlen(clean);
    if (utf8_cols(clean, n) > cols)
        n = utf8_take_cols(clean, n, cols - 1, &truncated);
    int width = utf8_cols(clean, n) + (truncated ? 1 : 0);
    raw("\0337");
    position(rows - 1, 0);
    raw(g_cfg.color ? "\033[0;1;7m" : "\033[0m");
    fwrite(clean, 1, n, stdout);
    if (truncated) raw("…");
    /* Explicit spaces, including the final column: do not depend on EL's
     * background/reverse-video behavior. No printable byte follows the last
     * cell, so pending autowrap cannot scroll the footer. */
    for (int i = width; i < cols; i++) fputc(' ', stdout);
    raw("\0338");
    sb_free(&text);
    dirty = 0;
}

static void dimensions(void)
{
    int r = term_rows(), c = term_cols();
    if (r < 2) r = 2;
    if (c < 2) c = 2;
    if (r == rows && c == cols) return;
    /* Match Term49's resize anchoring, including the case where there is
     * not yet enough history to reveal a full screenful on expansion. */
    int shift = 0;
    if (rows && r > rows) {
        shift = r - rows;
        if (history_top < shift) shift = (int)history_top;
    } else if (rows && row >= r) shift = r - row - 1;
    int old_footer = rows - 1 + shift;
    row += shift;
    history_top -= shift;
    if (rows && old_footer >= 0 && old_footer < r && old_footer != r - 1) {
        raw("\0337");
        position(old_footer, 0);
        raw("\033[0m\033[2K\0338");
    }
    rows = r;
    cols = c;
    if (row >= rows - 1) {
        /* Shrinking can put live content on the new physical bottom.
         * Archive a row WITHOUT clearing that content, then reserve the
         * newly exposed blank bottom row for the footer. */
        raw("\0337");
        position(rows - 1, 0);
        raw("\r\n\0338");
        history_top++;
        row = rows - 2;
    }
    if (col >= cols) col = cols - 1;
    position(row, col);
    dirty = 1;
}

static void newline(void)
{
    if (row >= rows - 2) {
        raw("\0337");
        position(rows - 1, 0);
        raw("\033[0m\033[2K\r\n\0338\r");
        history_top++;
        dirty = 1;
    } else {
        raw("\r\n");
        row++;
    }
    col = 0;
}

static void character(const char *s, size_t n)
{
    size_t used;
    int width = uc_width(utf8_decode(s, n, &used));
    if (width > 0 && col + width > cols) newline();
    fwrite(s, 1, n, stdout);
    col += width;
}

static void control(void)
{
    escape[ne] = 0;
    char final = escape[ne - 1];
    if (ne >= 3 && escape[1] == '[') {
        int a = 0, b = 0;
        sscanf(escape + 2, "%d;%d", &a, &b);
        int amount = a > 0 ? a : 1;
        switch (final) {
        case 'A': row -= amount; break;
        case 'B': row += amount; break;
        case 'C': col += amount; break;
        case 'D': col -= amount; break;
        case 'H': case 'f': row = (a > 0 ? a : 1) - 1; col = (b > 0 ? b : 1) - 1; break;
        case 'J': dirty = 1; break;
        default: break;  /* SGR, erase-line and bracketed-paste modes */
        }
        if (row < 0) row = 0;
        if (row > rows - 2) row = rows - 2;
        if (col < 0) col = 0;
        if (final == 'A' || final == 'B' || final == 'C' || final == 'D' ||
            final == 'H' || final == 'f') {
            if (col >= cols) col = cols - 1;
            position(row, col);
        }
        else raw(escape);
    }
    ne = 0;
}

static void byte(unsigned char c)
{
    if (ne) {
        if (ne >= sizeof escape - 1) { ne = 0; return; }
        escape[ne++] = (char)c;
        if ((ne == 2 && c != '[') || (ne > 2 && c >= 0x40 && c <= 0x7e)) control();
        return;
    }
    if (nu) {
        if ((c & 0xc0) == 0x80) {
            unicode[nu++] = (char)c;
            if (nu == want) { character(unicode, nu); nu = 0; }
            return;
        }
        character("?", 1);
        nu = 0;
    }
    if (c == 27) { escape[ne++] = (char)c; return; }
    if (c == '\n') { newline(); return; }
    if (c == '\r') { raw("\r"); col = 0; return; }
    if (c == '\b') { if (col > 0) col--; raw("\b"); return; }
    if (c == '\t') {
        int spaces = 8 - col % 8;
        while (spaces--) character(" ", 1);
        return;
    }
    if (c < 32 || c == 127) return;
    if (c >= 0xc2 && c <= 0xf4) {
        unicode[0] = (char)c;
        nu = 1;
        want = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    } else if (c >= 128) character("?", 1);
    else { char ch = (char)c; character(&ch, 1); }
}

void term_write(const char *s, size_t n)
{
    pthread_mutex_lock(&lock);
    if (!active) fwrite(s, 1, n, stdout);
    else {
        int bracket = g_cfg.sync_output && !sync_depth;
        if (bracket) raw("\033[?2026h");
        dimensions();
        for (size_t i = 0; i < n; i++) byte((unsigned char)s[i]);
        if (dirty) paint();
        if (bracket) raw("\033[?2026l");
    }
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

int term_printf(const char *fmt, ...)
{
    sbuf s = {0};
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&s, fmt, ap);
    va_end(ap);
    int n = (int)s.n;
    term_write(sb_str(&s), s.n);
    sb_free(&s);
    return n;
}

int term_fputs(const char *s, FILE *stream)
{
    if (stream != stdout) return fputs(s, stream);
    term_write(s, strlen(s));
    return 0;
}

int term_fputc(int c, FILE *stream)
{
    if (stream != stdout) return fputc(c, stream);
    char ch = (char)c;
    term_write(&ch, 1);
    return (unsigned char)c;
}

size_t term_fwrite(const void *p, size_t size, size_t n, FILE *stream)
{
    if (stream != stdout) return fwrite(p, size, n, stream);
    if (!size || !n) return 0;
    if (size && n > (size_t)-1 / size) return 0;
    term_write(p, size * n);
    return n;
}

void sync_begin(void)
{
    pthread_mutex_lock(&lock);
    if (active && g_cfg.sync_output && sync_depth++ == 0) raw("\033[?2026h");
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

void sync_end(void)
{
    pthread_mutex_lock(&lock);
    if (active && g_cfg.sync_output && sync_depth > 0 && --sync_depth == 0) raw("\033[?2026l");
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

void fs_enter(void)
{
    if (!isatty(1)) return;
    pthread_mutex_lock(&lock);
    raw("\033[?1049h\033[r\033[H");
    active = 1;
    rows = cols = row = col = sync_depth = 0;
    history_top = 0;
    ne = nu = 0;
    dimensions();
    paint();
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

void fs_leave(void)
{
    pthread_mutex_lock(&lock);
    if (active) raw("\033[?2026l\033[0m\033[r\033[?1049l");
    active = 0;
    fflush(stdout);
    pthread_mutex_unlock(&lock);
}

void fs_repaint(void)
{
    pthread_mutex_lock(&lock);
    dirty = 1;
    pthread_mutex_unlock(&lock);
    term_write("", 0);
}

void fs_set_tokens(long in, long out)
{
    pthread_mutex_lock(&lock);
    tokens_in = in > 0 ? in : 0;
    tokens_out = out > 0 ? out : 0;
    dirty = 1;
    pthread_mutex_unlock(&lock);
    term_write("", 0);
}
