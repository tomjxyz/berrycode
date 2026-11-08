/* berrycode - a small line editor for the prompt.
 *
 * The terminal's own (canonical) line editing can't backspace across a wrapped line,
 * so on a tty we read keys in raw mode and redraw the input ourselves:
 *   Backspace, Delete, Left/Right, Home/End (Ctrl-A/E), Up/Down history,
 *   Ctrl-U / Ctrl-K kill to start / end, Ctrl-W delete word, Ctrl-D (EOF on empty line).
 * Ctrl-C still raises SIGINT (ISIG stays on) and is reported via *interrupted.
 */
#include "bc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HIST_MAX 100

static char *hist[HIST_MAX];
static int   nhist;

typedef struct {
    sbuf        b;          /* the text */
    size_t      pos;        /* cursor, byte offset (always on a UTF-8 boundary) */
    const char *prompt;     /* printed as-is (may contain colour codes) */
    int         pcols;      /* visible width of the prompt */
    int         crow;       /* row of the cursor relative to the prompt's first row */
    int         erows;      /* rows the field spanned at the last refresh (bounded erase) */
} ed;

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* visible columns (CJK/emoji count 2, matching Term49CX's double-width
 * cells; combining marks 0). Byte-boundary motion still uses is_cont. */
static int cols_of(const char *s, size_t n)
{
    return utf8_cols(s, n);
}

static void out(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        s += w;
        n -= (size_t)w;
    }
}

/* redraw prompt + text and put the cursor where it belongs */
static void refresh(ed *e)
{
    int cols = term_cols();
    int total = e->pcols + cols_of(sb_str(&e->b), e->b.n);
    int cur = e->pcols + cols_of(sb_str(&e->b), e->pos);
    sbuf o = {0};
    /* Back to the field's first row, then erase exactly the rows the
     * field occupied at the last refresh, bottom-up. Never touches rows
     * below the field: in fullscreen the status bar lives there. */
    if (e->crow > 0) sb_printf(&o, "\033[%dA", e->crow);
    if (e->erows > 0) sb_printf(&o, "\033[%dB", e->erows);
    for (int i = e->erows; i >= 0; i--) {
        sb_adds(&o, "\r\033[K");
        if (i > 0) sb_adds(&o, "\033[A");
    }
    sb_adds(&o, e->prompt);
    sb_adds(&o, C_BOLD);                       /* typed input stands out */
    sb_add(&o, sb_str(&e->b), e->b.n);
    sb_adds(&o, C_RESET);
    /* text ending exactly at the right margin leaves the cursor in "pending wrap";
     * move it onto the next row explicitly so the row arithmetic below holds */
    if (total > 0 && total % cols == 0) sb_adds(&o, "\r\n");
    int end_row = total / cols, cur_row = cur / cols, cur_col = cur % cols;
    if (end_row > cur_row) sb_printf(&o, "\033[%dA", end_row - cur_row);
    sb_adds(&o, "\r");
    if (cur_col) sb_printf(&o, "\033[%dC", cur_col);
    out(o.p, o.n);
    sb_free(&o);
    e->crow = cur_row;
    e->erows = end_row;
}

static void set_text(ed *e, const char *s)
{
    e->b.n = 0;
    sb_adds(&e->b, s);
    e->pos = e->b.n;
}

static void insert(ed *e, const char *s, size_t n)
{
    sb_add(&e->b, s, n);                            /* grow, then shift into place */
    memmove(e->b.p + e->pos + n, e->b.p + e->pos, e->b.n - n - e->pos);
    memcpy(e->b.p + e->pos, s, n);
    e->pos += n;
    e->b.p[e->b.n] = 0;
}

static void erase(ed *e, size_t from, size_t to)    /* remove bytes [from, to) */
{
    if (to <= from) return;
    memmove(e->b.p + from, e->b.p + to, e->b.n - to);
    e->b.n -= to - from;
    e->b.p[e->b.n] = 0;
    if (e->pos > to) e->pos -= to - from;
    else if (e->pos > from) e->pos = from;
}

static size_t prev_char(ed *e, size_t p)
{
    if (p == 0) return 0;
    p--;
    while (p > 0 && is_cont((unsigned char)e->b.p[p])) p--;
    return p;
}

static size_t next_char(ed *e, size_t p)
{
    if (p >= e->b.n) return e->b.n;
    p++;
    while (p < e->b.n && is_cont((unsigned char)e->b.p[p])) p++;
    return p;
}

static void hist_add(const char *s)
{
    if (!*s || (nhist && !strcmp(hist[nhist - 1], s))) return;
    if (nhist == HIST_MAX) { free(hist[0]); memmove(hist, hist + 1, sizeof(char *) * (HIST_MAX - 1)); nhist--; }
    hist[nhist++] = xstrdup(s);
}

static int read_byte(unsigned char *c)
{
    ssize_t r = read(0, c, 1);
    if (r == 1) return 1;
    if (r < 0 && errno == EINTR) return -2;       /* signal (Ctrl-C) */
    return 0;                                      /* EOF / error */
}

static char *edit_line(const char *prompt, int pcols, int *interrupted, int use_hist)
{
    ed e;
    memset(&e, 0, sizeof e);
    e.prompt = prompt;
    e.pcols = pcols;
    sb_add(&e.b, "", 0);
    int hidx = nhist;               /* history cursor; nhist = the line being typed */
    char *saved = NULL;             /* the line being typed while browsing history */
    char *result = NULL;
    /* bracketed paste (Term49CX supports 2004): pastes arrive as one
     * ESC[200~ ... ESC[201~ block. Non-supporting terminals ignore the
     * mode switch and everything still works, just without atomicity. */
    int do_paste = isatty(1);
    refresh(&e);
    if (do_paste) out("\033[?2004h", 8);
    for (;;) {
        unsigned char c;
        int r = read_byte(&c);
        if (r == -2) {                       /* EINTR: resize or Ctrl-C */
            if (g_winch_soft) {              /* VKB open/close: reflowed, forget rows */
                g_winch_soft = 0;
                e.crow = 0;
                e.erows = 0;
                fs_repaint();
                refresh(&e);
                continue;
            }
            if (interrupted) *interrupted = 1;
            break;
        }
        if (r == 0) { if (e.b.n) { result = sb_take(&e.b); } break; }
        if (c == '\r' || c == '\n') {
            e.pos = e.b.n;
            refresh(&e);
            bar_hide();                        /* the newline may scroll */
            out("\r\n", 2);
            result = sb_take(&e.b);
            if (use_hist) hist_add(result);
            break;
        } else if (c == 127 || c == 8) {                         /* backspace */
            erase(&e, prev_char(&e, e.pos), e.pos);
        } else if (c == 4) {                                     /* Ctrl-D */
            if (!e.b.n) break;
            erase(&e, e.pos, next_char(&e, e.pos));
        } else if (c == 1) e.pos = 0;                            /* Ctrl-A */
        else if (c == 5) e.pos = e.b.n;                          /* Ctrl-E */
        else if (c == 21) erase(&e, 0, e.pos);                   /* Ctrl-U */
        else if (c == 11) erase(&e, e.pos, e.b.n);               /* Ctrl-K */
        else if (c == 23) {                                      /* Ctrl-W */
            size_t p = e.pos;
            while (p > 0 && e.b.p[p - 1] == ' ') p--;
            while (p > 0 && e.b.p[p - 1] != ' ') p--;
            erase(&e, p, e.pos);
        } else if (c == 12) {                                    /* Ctrl-L: clear screen */
            out("\033[H\033[2J", 7);
            e.crow = 0;
            e.erows = 0;
            fs_repaint();              /* restore the status bar in fullscreen */
        } else if (c == '\t') {
            insert(&e, "    ", 4);
        } else if (c == 27) {                                    /* escape sequences */
            unsigned char a, b2;
            if (read_byte(&a) != 1) continue;
            if (a == 'O') {                                      /* SS3 (app-cursor keys) */
                if (read_byte(&b2) != 1) continue;
                if (b2 == 'H') e.pos = 0;
                else if (b2 == 'F') e.pos = e.b.n;
                else if (b2 == 'D') e.pos = prev_char(&e, e.pos);
                else if (b2 == 'C') e.pos = next_char(&e, e.pos);
                else if (use_hist && b2 == 'A') {
                    if (hidx > 0) {
                        if (hidx == nhist) { free(saved); saved = xstrdup(sb_str(&e.b)); }
                        set_text(&e, hist[--hidx]);
                    }
                } else if (use_hist && b2 == 'B' && hidx < nhist) {
                    hidx++;
                    set_text(&e, hidx == nhist ? (saved ? saved : "") : hist[hidx]);
                }
                /* F1-F4 (P/Q/R/S) and friends: ignore */
            } else if (a != '[') {
                continue;
            } else if (read_byte(&b2) != 1) {
                continue;
            } else if (b2 >= '0' && b2 <= '9') {                 /* ESC [ params final */
                char digits[16];
                int nd = 0, mods = 0;
                unsigned char t = 0;
                digits[nd++] = (char)b2;
                for (;;) {                                       /* collect until final byte */
                    if (read_byte(&t) != 1) break;
                    if (t >= '0' && t <= '9') { if (nd < 15) digits[nd++] = (char)t; }
                    else if (t == ';') mods = 1;                 /* modified key: still honor final */
                    else break;
                }
                digits[nd] = 0;
                if (t == '~' && !mods) {
                    if (!strcmp(digits, "200")) {
                        /* ---- bracketed paste: one atomic insert ----
                         * Newlines arrive as pasted line breaks, but this
                         * prompt is a single visual line, so they become
                         * spaces (use """ blocks for real multi-line).
                         * Tabs become 4 spaces like the Tab key. */
                        sbuf pb = {0};
                        int esc = 0;
                        char pd[16];
                        int pnd = 0;
                        int pr;
                        unsigned char pc;
                        while ((pr = read_byte(&pc)) == 1) {
                            if (esc == 0) {
                                if (pc == 0x1b) esc = 1;
                                else if (pc == '\r' || pc == '\n') sb_addc(&pb, ' ');
                                else if (pc == '\t') sb_adds(&pb, "    ");
                                else if (pc >= 32) sb_addc(&pb, (char)pc);
                                /* other controls: drop */
                            } else if (esc == 1) {
                                if (pc == '[') { esc = 2; pnd = 0; }
                                else esc = 0;                        /* lone ESC: drop */
                            } else {
                                if (pc >= '0' && pc <= '9') { if (pnd < 15) pd[pnd++] = (char)pc; }
                                else {
                                    pd[pnd] = 0;
                                    esc = 0;
                                    if (pc == '~' && !strcmp(pd, "201")) break;  /* end of paste */
                                    /* any other escape inside: drop */
                                }
                            }
                        }
                        if (pb.n) insert(&e, pb.p, pb.n);
                        sb_free(&pb);
                        if (pr == -2) {                          /* signal inside paste */
                            if (g_winch_soft) {
                                g_winch_soft = 0;
                                e.crow = 0;
                                e.erows = 0;
                                fs_repaint();
                                refresh(&e);
                                continue;
                            }
                            if (interrupted) *interrupted = 1;
                            break;
                        }
                        /* EOF (pr == 0) inside paste: just end the paste */
                    } else if (!strcmp(digits, "201")) {
                        /* stray close marker: ignore */
                    } else if (!strcmp(digits, "3")) {
                        erase(&e, e.pos, next_char(&e, e.pos));  /* Delete */
                    } else if (!strcmp(digits, "1") || !strcmp(digits, "7")) {
                        e.pos = 0;                               /* Home */
                    } else if (!strcmp(digits, "4") || !strcmp(digits, "8")) {
                        e.pos = e.b.n;                           /* End */
                    }
                    /* anything else (5~/6~ PgUp/PgDn, 11~-24~ Fx): ignore */
                } else if (!mods || t == 'A' || t == 'B' || t == 'C' ||
                           t == 'D' || t == 'H' || t == 'F') {
                    /* normal or shifted arrows/Home/End: still move */
                    if (t == 'D') e.pos = prev_char(&e, e.pos);
                    else if (t == 'C') e.pos = next_char(&e, e.pos);
                    else if (t == 'H') e.pos = 0;
                    else if (t == 'F') e.pos = e.b.n;
                    else if (use_hist && t == 'A') {
                        if (hidx > 0) {
                            if (hidx == nhist) { free(saved); saved = xstrdup(sb_str(&e.b)); }
                            set_text(&e, hist[--hidx]);
                        }
                    } else if (use_hist && t == 'B' && hidx < nhist) {
                        hidx++;
                        set_text(&e, hidx == nhist ? (saved ? saved : "") : hist[hidx]);
                    }
                }
            } else {
                if (b2 == 'D') e.pos = prev_char(&e, e.pos);
                else if (b2 == 'C') e.pos = next_char(&e, e.pos);
                else if (b2 == 'H') e.pos = 0;
                else if (b2 == 'F') e.pos = e.b.n;
                else if (use_hist && (b2 == 'A' || b2 == 'B')) {
                    if (b2 == 'A' && hidx > 0) {
                        if (hidx == nhist) { free(saved); saved = xstrdup(sb_str(&e.b)); }
                        set_text(&e, hist[--hidx]);
                    } else if (b2 == 'B' && hidx < nhist) {
                        hidx++;
                        set_text(&e, hidx == nhist ? (saved ? saved : "") : hist[hidx]);
                    }
                }
            }
        } else if (c >= 32 && c != 127) {
            int at_end = e.pos == e.b.n;
            insert(&e, (const char *)&c, 1);
            if (c >= 0xC0) continue;                             /* UTF-8 lead byte: wait for the rest */
            /* plain ASCII typed at the end, not reaching the margin: just echo (fast on a slow phone) */
            if (c < 0x80 && at_end && (e.pcols + cols_of(e.b.p, e.b.n)) % term_cols() != 0) {
                out((const char *)&c, 1);
                continue;
            }
        } else {
            continue;                                            /* other control keys: ignore */
        }
        refresh(&e);
    }
    if (do_paste) out("\033[?2004l", 8);   /* leave bracketed-paste mode */
    free(saved);
    if (!result) sb_free(&e.b);
    return result;
}

char *prompt_line(const char *prompt, int pcols, int use_hist, int *interrupted)
{
    if (interrupted) *interrupted = 0;
    struct termios orig;
    if (!g_cfg.line_editor || !isatty(0) || tcgetattr(0, &orig) != 0) {
        fputs(prompt, stdout);
        fflush(stdout);
        return read_line(stdin, interrupted);
    }
    fflush(stdout);
    struct termios raw = orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);   /* keep ISIG: Ctrl-C -> SIGINT */
    raw.c_iflag &= ~(tcflag_t)(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSADRAIN, &raw);
    char *r = edit_line(prompt, pcols, interrupted, use_hist);
    tcsetattr(0, TCSADRAIN, &orig);
    return r;
}
