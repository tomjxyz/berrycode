/* berrycode - the tools the model can call, plus the approval prompt */
#include "bc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

#define READ_DEFAULT_LINES 400
#define READ_MAX_BYTES     (48 * 1024)
#define LINE_MAX_CHARS     500
#define OUT_MAX_BYTES      (32 * 1024)
#define GREP_MAX_MATCHES   200
#define GLOB_MAX_RESULTS   300
#define LIST_MAX_ENTRIES   400
#define WALK_MAX_FILES     20000
#define PREVIEW_LINES      30

static int always_write, always_bash;   /* "a" answers, for this session */

void tools_reset_session(void) { always_write = always_bash = 0; }

static const char *TOOLS_JSON =
"["
"{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
 "\"description\":\"Read a text file. Returns numbered lines (\\\"N| text\\\"). Use offset/limit for big files.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path (relative to the working directory or absolute)\"},"
  "\"offset\":{\"type\":\"integer\",\"description\":\"1-based line to start at (default 1)\"},"
  "\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return (default 400)\"}},"
 "\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"list_dir\","
 "\"description\":\"List a directory (dirs end with /, files show size).\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"Directory (default .)\"}}}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"glob\","
 "\"description\":\"Recursively find files whose name or relative path matches a shell glob, e.g. *.c or src/*.h.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"pattern\":{\"type\":\"string\"},"
  "\"path\":{\"type\":\"string\",\"description\":\"Directory to search (default .)\"}},"
 "\"required\":[\"pattern\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"grep\","
 "\"description\":\"Search file contents recursively with a POSIX extended regex. Returns file:line: text.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"pattern\":{\"type\":\"string\",\"description\":\"Extended regular expression\"},"
  "\"path\":{\"type\":\"string\",\"description\":\"File or directory (default .)\"},"
  "\"include\":{\"type\":\"string\",\"description\":\"Only files whose name matches this glob, e.g. *.c\"},"
  "\"ignore_case\":{\"type\":\"boolean\"}},"
 "\"required\":[\"pattern\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"write_file\","
 "\"description\":\"Create or overwrite a file with the given content (parent dirs are created). Prefer edit_file for changes to existing files.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
 "\"required\":[\"path\",\"content\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"edit_file\","
 "\"description\":\"Replace an exact string in a file. old_string must match exactly (including whitespace) and be unique unless replace_all is true. Read the file first.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\"},"
  "\"old_string\":{\"type\":\"string\"},"
  "\"new_string\":{\"type\":\"string\"},"
  "\"replace_all\":{\"type\":\"boolean\"}},"
 "\"required\":[\"path\",\"old_string\",\"new_string\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"bash\","
 "\"description\":\"Run a command with /bin/sh -c in the working directory (QNX shell; stdin is /dev/null). Returns combined stdout+stderr and the exit code.\","
 "\"parameters\":{\"type\":\"object\",\"properties\":{"
  "\"command\":{\"type\":\"string\"},"
  "\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds before the command is killed\"}},"
 "\"required\":[\"command\"]}}}"
"]";

json *tools_schema(void)
{
    json *j = json_parse(TOOLS_JSON, strlen(TOOLS_JSON), NULL);
    if (!j) { fprintf(stderr, "berrycode: internal error: bad tool schema\n"); exit(2); }
    /* put the configured limits into the descriptions so the model knows them */
    for (int i = 0; i < json_len(j); i++) {
        json *fn = json_get(json_at(j, i), "function");
        const char *name = json_gets(fn, "name");
        json *props = json_get(json_get(fn, "parameters"), "properties");
        sbuf d = {0};
        if (name && !strcmp(name, "bash")) {
            sb_printf(&d, "Seconds before the command is killed (default %d, max %d). This is a slow phone: "
                      "avoid commands that scan the whole filesystem.", g_cfg.bash_timeout, g_cfg.bash_timeout_max);
            json_set(json_get(props, "timeout"), "description", json_str(d.p));
        } else if (name && (!strcmp(name, "glob") || !strcmp(name, "grep"))) {
            sb_printf(&d, "%s Searches stop after %d seconds, so prefer a specific path over / or large trees.",
                      json_gets(fn, "description"), g_cfg.search_timeout);
            json_set(fn, "description", json_str(d.p));
        }
        sb_free(&d);
    }
    return j;
}

/* ---------- helpers ---------- */

static char *errf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *errf(const char *fmt, ...)
{
    sbuf b = {0};
    sb_adds(&b, "Error: ");
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    return sb_take(&b);
}

static int is_binary(const char *data, size_t n)
{
    size_t k = n < 4096 ? n : 4096;
    return memchr(data, 0, k) != NULL;
}

static int count_lines(const char *s)
{
    if (!*s) return 0;
    int n = 0;
    for (const char *p = s; *p; p++) if (*p == '\n') n++;
    if (s[strlen(s) - 1] != '\n') n++;
    return n;
}

/* print lines of text with a prefix, at most max lines */
static void print_lines(const char *text, size_t len, const char *prefix, const char *color, int max)
{
    sync_begin();              /* one Term49CX frame for the whole block */
    const char *p = text, *end = text + len;
    int shown = 0, total = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (shown < max) {
            /* never fill the last column: simple terminals mishandle full-width lines */
            int w = term_cols() - (int)strlen(prefix) - 1;
            if (w < 16) w = 16;
            char *clean = term_clean(p, ll);
            size_t cl = strlen(clean), cut = cl;
            int truncated = utf8_cols(clean, cl) > w;
            if (truncated) {                     /* keep w-1 columns, then "…" */
                int k = 0;
                cut = 0;
                while (cut < cl) {
                    size_t dl = 0;
                    unsigned cp = utf8_decode(clean + cut, cl - cut, &dl);
                    if (!dl) break;
                    int cw = uc_width(cp);
                    if (k + cw > w - 1) break;   /* wide char: wrap before it */
                    k += cw;
                    cut += dl;
                }
            }
            printf("%s%s%.*s%s%s\n", color, prefix, (int)cut, clean, truncated ? "…" : "", C_RESET);
            free(clean);
            shown++;
        }
        total++;
        p = nl ? nl + 1 : end;
    }
    if (total > shown) printf("%s  … %d more line(s)%s\n", C_DIM, total - shown, C_RESET);
    sync_end();
}

/* print text wrapped to the screen width (nothing cut off sideways); max = screen lines, 0 = all */
static void print_wrapped(const char *raw, const char *first, const char *cont, const char *color, int max)
{
    sync_begin();
    int w = term_cols() - (int)strlen(first) - 1;
    if (w < 16) w = 16;
    char *text = term_clean(raw, strlen(raw));
    const char *p = text, *end = text + strlen(text);
    int shown = 0, hidden = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        do {                                       /* one source line -> one or more screen lines */
            /* take up to w display columns (wide CJK counts 2, combining 0) */
            size_t n = 0;
            int cols = 0;
            while (p + n < le) {
                size_t dl = 0;
                unsigned cp = utf8_decode(p + n, (size_t)(le - (p + n)), &dl);
                if (!dl) break;
                int cw = uc_width(cp);
                if (cols + cw > w) break;
                cols += cw;
                n += dl;
            }
            if (!max || shown < max) {
                printf("%s%s%.*s%s\n", color, shown ? cont : first, (int)n, p, C_RESET);
                shown++;
            } else hidden++;
            p += n;
        } while (p < le);
        p = nl ? nl + 1 : end;
    }
    if (hidden) printf("%s    … %d more line(s) not shown (bash_command_lines)%s\n", C_DIM, hidden, C_RESET);
    free(text);
    sync_end();
}

/* show a compact before/after diff: common prefix/suffix lines are skipped */
static void print_diff(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t pre = 0, line_start = 0;
    int line_no = 1;
    while (pre < la && pre < lb && a[pre] == b[pre]) {
        if (a[pre] == '\n') { line_start = pre + 1; line_no++; }
        pre++;
    }
    pre = line_start;
    size_t sa = la, sb = lb;
    while (sa > pre && sb > pre && a[sa - 1] == b[sb - 1]) { sa--; sb--; }
    /* extend suffix boundary to a line end */
    while (sa < la && a[sa] != '\n') { sa++; sb++; }
    if (sa < la) { sa++; sb++; }
    if (sb > lb) sb = lb;
    printf("%s  @@ line %d @@%s\n", C_CYAN, line_no, C_RESET);
    print_lines(a + pre, sa - pre, "- ", C_RED, PREVIEW_LINES);
    print_lines(b + pre, sb - pre, "+ ", C_GREEN, PREVIEW_LINES);
}

typedef enum { ASK_YES, ASK_NO } ask_result;

/* ask the user; on deny, *reason gets optional free text from the user */
static ask_result ask_approval(int *always_flag, const char *what, char **reason)
{
    *reason = NULL;
    if (g_cfg.yolo || *always_flag) return ASK_YES;
    if (!isatty(0)) {
        *reason = xstrdup("not approved: berrycode is running non-interactively (use --yolo to allow)");
        return ASK_NO;
    }
    for (;;) {
        printf("%s? Allow %s?%s [Y]es (Enter) / [n]o / [a]lways this session / or type what to do instead\n",
               C_YELLOW, what, C_RESET);
        int intr = 0;
        char *ans = prompt_line(g_cfg.color ? "\033[33m> \033[0m" : "> ", 2, 0, &intr);
        if (!ans) {
            printf("\n");
            g_interrupted = 0;
            return ASK_NO;
        }
        char *a = ans;
        while (*a == ' ') a++;
        if (!*a) { free(ans); return ASK_YES; }     /* plain Enter = yes */
        if (!strcasecmp(a, "y") || !strcasecmp(a, "yes")) { free(ans); return ASK_YES; }
        if (!strcasecmp(a, "a") || !strcasecmp(a, "always")) { *always_flag = 1; free(ans); return ASK_YES; }
        if (!strcasecmp(a, "n") || !strcasecmp(a, "no")) { free(ans); return ASK_NO; }
        *reason = xstrdup(a);
        free(ans);
        return ASK_NO;
    }
}

static char *denied(char *reason)
{
    sbuf b = {0};
    sb_adds(&b, "The user denied this action.");
    if (reason) sb_printf(&b, " They said: %s", reason);
    free(reason);
    return sb_take(&b);
}

static void tool_header(const char *name, const char *arg)
{
    printf("%s● %s%s %s%s\n", C_MAGENTA, name, C_RESET, arg ? arg : "", "");
}

/* ---------- read-only tools ---------- */

static char *t_read_file(const json *a)
{
    const char *path = json_gets(a, "path");
    if (!path || !*path) return errf("path is required");
    int offset = (int)json_getn(a, "offset", 1), limit = (int)json_getn(a, "limit", READ_DEFAULT_LINES);
    if (offset < 1) offset = 1;
    if (limit < 1) limit = READ_DEFAULT_LINES;
    tool_header("read", path);
    struct stat st;
    if (stat(path, &st) != 0) return errf("%s: %s", path, strerror(errno));
    if (S_ISDIR(st.st_mode)) return errf("%s is a directory (use list_dir)", path);
    size_t n;
    char *data = read_file_all(path, &n);
    if (!data) return errf("cannot read %s: %s", path, strerror(errno));
    if (is_binary(data, n)) { free(data); return errf("%s looks like a binary file (%zu bytes)", path, n); }
    sbuf out = {0};
    int line = 1, shown = 0, total = count_lines(data);
    const char *p = data, *end = data + n;
    int truncated = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line >= offset && shown < limit) {
            if (out.n > READ_MAX_BYTES) { truncated = 1; break; }
            sb_printf(&out, "%d| ", line);
            if (ll > LINE_MAX_CHARS) { sb_add(&out, p, LINE_MAX_CHARS); sb_adds(&out, " [line truncated]"); }
            else sb_add(&out, p, ll);
            sb_addc(&out, '\n');
            shown++;
        }
        line++;
        p = nl ? nl + 1 : end;
    }
    free(data);
    if (total == 0) sb_adds(&out, "(empty file)\n");
    int last = offset + shown - 1;
    if (shown && (offset > 1 || last < total || truncated))
        sb_printf(&out, "[showing lines %d-%d of %d; use offset to read more]\n", offset, last, total);
    printf("%s  %d line(s)%s\n", C_DIM, shown, C_RESET);
    return sb_take(&out);
}

static char *t_list_dir(const json *a)
{
    const char *path = json_gets(a, "path");
    if (!path || !*path) path = ".";
    tool_header("ls", path);
    DIR *d = opendir(path);
    if (!d) return errf("%s: %s", path, strerror(errno));
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = xrealloc(names, sizeof(char *) * (size_t)cap); }
        names[n++] = xstrdup(e->d_name);
    }
    closedir(d);
    /* simple insertion sort - directories are small */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    sbuf out = {0};
    for (int i = 0; i < n; i++) {
        if (i < LIST_MAX_ENTRIES) {
            sbuf full = {0};
            sb_printf(&full, "%s/%s", path, names[i]);
            struct stat st;
            if (lstat(full.p, &st) != 0) sb_printf(&out, "%s  (?)\n", names[i]);
            else if (S_ISDIR(st.st_mode)) sb_printf(&out, "%s/\n", names[i]);
            else if (S_ISLNK(st.st_mode)) sb_printf(&out, "%s@\n", names[i]);
            else sb_printf(&out, "%s  (%lld bytes)\n", names[i], (long long)st.st_size);
            sb_free(&full);
        }
        free(names[i]);
    }
    free(names);
    if (n > LIST_MAX_ENTRIES) sb_printf(&out, "[%d more entries not shown]\n", n - LIST_MAX_ENTRIES);
    if (n == 0) sb_adds(&out, "(empty directory)\n");
    printf("%s  %d entr%s%s\n", C_DIM, n, n == 1 ? "y" : "ies", C_RESET);
    return sb_take(&out);
}

/* recursive walk; cb returns nonzero to stop */
typedef int (*walk_cb)(const char *path, const char *rel, void *ud);

static double walk_deadline;
static int    walk_timed_out;

static double now_s(void)            /* monotonic seconds (time() only has 1 s resolution) */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void walk_begin(void)
{
    walk_deadline = now_s() + g_cfg.search_timeout;
    walk_timed_out = 0;
}

static void walk_note(sbuf *out)
{
    if (walk_timed_out)
        sb_printf(out, "[search stopped after %d s timeout - results are incomplete; use a narrower path or pattern]\n",
                  g_cfg.search_timeout);
}

static int skip_dir(const char *name)
{
    return !strcmp(name, ".git") || !strcmp(name, ".svn") || !strcmp(name, ".hg") ||
           !strcmp(name, "node_modules") || !strcmp(name, "__pycache__");
}

static int walk(const char *root, const char *rel, walk_cb cb, void *ud, int *budget, int depth)
{
    if (depth > 30) return 0;
    sbuf dir = {0};
    if (*rel) sb_printf(&dir, "%s/%s", root, rel); else sb_adds(&dir, root);
    DIR *d = opendir(dir.p);
    if (!d) { sb_free(&dir); return 0; }
    struct dirent *e;
    int stop = 0;
    while (!stop && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (--*budget < 0) { stop = 1; break; }
        if (now_s() >= walk_deadline) { walk_timed_out = 1; stop = 1; break; }
        sbuf r = {0}, full = {0};
        if (*rel) sb_printf(&r, "%s/%s", rel, e->d_name); else sb_adds(&r, e->d_name);
        sb_printf(&full, "%s/%s", root, r.p);
        struct stat st;
        if (lstat(full.p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!skip_dir(e->d_name)) stop = walk(root, r.p, cb, ud, budget, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                stop = cb(full.p, r.p, ud);
            }
        }
        sb_free(&r);
        sb_free(&full);
    }
    closedir(d);
    sb_free(&dir);
    return stop || g_interrupted;
}

typedef struct { const char *pat; sbuf out; int count; } glob_ud;

static int glob_cb(const char *path, const char *rel, void *ud)
{
    (void)path;
    glob_ud *g = ud;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    int m = strchr(g->pat, '/') ? fnmatch(g->pat, rel, FNM_PATHNAME) == 0 : fnmatch(g->pat, base, 0) == 0;
    if (m) {
        if (g->count < GLOB_MAX_RESULTS) sb_printf(&g->out, "%s\n", rel);
        g->count++;
    }
    return 0;
}

static char *t_glob(const json *a)
{
    const char *pat = json_gets(a, "pattern"), *path = json_gets(a, "path");
    if (!pat || !*pat) return errf("pattern is required");
    if (!path || !*path) path = ".";
    sbuf h = {0};
    sb_printf(&h, "%s in %s", pat, path);
    tool_header("glob", h.p);
    sb_free(&h);
    /* "**" isn't special in fnmatch; "**" + "/" is treated as any directory prefix */
    const char *p = pat;
    while (!strncmp(p, "**/", 3)) p += 3;
    glob_ud g = { p, {0}, 0 };
    int budget = WALK_MAX_FILES;
    walk_begin();
    walk(path, "", glob_cb, &g, &budget, 0);
    walk_note(&g.out);
    if (g.count > GLOB_MAX_RESULTS) sb_printf(&g.out, "[%d more matches not shown]\n", g.count - GLOB_MAX_RESULTS);
    if (budget < 0) sb_adds(&g.out, "[search stopped: too many files]\n");
    if (!g.count) sb_adds(&g.out, "(no matches)\n");
    printf("%s  %d match(es)%s%s\n", C_DIM, g.count, walk_timed_out ? " (stopped: timeout)" : "", C_RESET);
    return sb_take(&g.out);
}

typedef struct { regex_t re; const char *include; sbuf out; int count, files; } grep_ud;

static int grep_file(const char *path, const char *rel, grep_ud *g)
{
    if (g->include) {
        const char *base = strrchr(rel, '/');
        base = base ? base + 1 : rel;
        if (fnmatch(g->include, base, 0) != 0) return 0;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size > 4 * 1024 * 1024) return 0;
    size_t n;
    char *data = read_file_all(path, &n);
    if (!data) return 0;
    if (is_binary(data, n)) { free(data); return 0; }
    g->files++;
    char *p = data, *end = data + n;
    int line = 1;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char save = 0;
        if (nl) { save = *nl; *nl = 0; }
        if (regexec(&g->re, p, 0, NULL, 0) == 0) {
            if (g->count < GREP_MAX_MATCHES) {
                size_t ll = strlen(p);
                sb_printf(&g->out, "%s:%d: %.*s%s\n", rel, line,
                          (int)(ll > 300 ? 300 : ll), p, ll > 300 ? "…" : "");
            }
            g->count++;
        }
        if (nl) *nl = save;
        line++;
        p = nl ? nl + 1 : end;
    }
    free(data);
    return g->count > GREP_MAX_MATCHES * 5;
}

static int grep_cb(const char *path, const char *rel, void *ud) { return grep_file(path, rel, ud); }

static char *t_grep(const json *a)
{
    const char *pat = json_gets(a, "pattern"), *path = json_gets(a, "path");
    if (!pat || !*pat) return errf("pattern is required");
    if (!path || !*path) path = ".";
    sbuf h = {0};
    sb_printf(&h, "/%s/ in %s", pat, path);
    tool_header("grep", h.p);
    sb_free(&h);
    grep_ud g;
    memset(&g, 0, sizeof g);
    g.include = json_gets(a, "include");
    if (g.include && !*g.include) g.include = NULL;
    int flags = REG_EXTENDED | REG_NOSUB | (json_getb(a, "ignore_case", 0) ? REG_ICASE : 0);
    int rc = regcomp(&g.re, pat, flags);
    if (rc) {
        char eb[200];
        regerror(rc, &g.re, eb, sizeof eb);
        return errf("bad regex: %s", eb);
    }
    struct stat st;
    if (stat(path, &st) != 0) { regfree(&g.re); return errf("%s: %s", path, strerror(errno)); }
    if (S_ISDIR(st.st_mode)) {
        int budget = WALK_MAX_FILES;
        walk_begin();
        walk(path, "", grep_cb, &g, &budget, 0);
        walk_note(&g.out);
        if (budget < 0) sb_adds(&g.out, "[search stopped: too many files]\n");
    } else {
        grep_file(path, path, &g);
    }
    regfree(&g.re);
    if (g.count > GREP_MAX_MATCHES) sb_printf(&g.out, "[%d+ matches total, only first %d shown]\n", g.count, GREP_MAX_MATCHES);
    if (!g.count) sb_printf(&g.out, "(no matches in %d file(s))\n", g.files);
    printf("%s  %d match(es)%s%s\n", C_DIM, g.count, walk_timed_out ? " (stopped: timeout)" : "", C_RESET);
    return sb_take(&g.out);
}

/* ---------- write tools ---------- */

static char *t_write_file(const json *a)
{
    const char *path = json_gets(a, "path");
    json *cj = json_get(a, "content");
    if (!path || !*path) return errf("path is required");
    if (!cj || cj->t != J_STR) return errf("content (string) is required");
    struct stat st;
    int exists = stat(path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return errf("%s is a directory", path);
    tool_header(exists ? "write (overwrite)" : "write (new file)", path);
    char *old = exists ? read_file_all(path, NULL) : NULL;
    if (old && !is_binary(old, strlen(old))) print_diff(old, cj->str);
    else print_lines(cj->str, cj->slen, "+ ", C_GREEN, PREVIEW_LINES);
    free(old);
    char *reason;
    if (ask_approval(&always_write, "this write", &reason) != ASK_YES) return denied(reason);
    if (mkdir_parents(path) != 0) return errf("cannot create parent directories of %s: %s", path, strerror(errno));
    if (write_file_all(path, cj->str, cj->slen) != 0) return errf("writing %s failed: %s", path, strerror(errno));
    printf("%s  wrote %zu bytes%s\n", C_DIM, cj->slen, C_RESET);
    sbuf r = {0};
    sb_printf(&r, "Wrote %zu bytes (%d lines) to %s", cj->slen, count_lines(cj->str), path);
    return sb_take(&r);
}

static char *t_edit_file(const json *a)
{
    const char *path = json_gets(a, "path");
    json *oj = json_get(a, "old_string"), *nj = json_get(a, "new_string");
    int all = json_getb(a, "replace_all", 0);
    if (!path || !*path) return errf("path is required");
    if (!oj || oj->t != J_STR || !nj || nj->t != J_STR) return errf("old_string and new_string (strings) are required");
    tool_header("edit", path);
    size_t n;
    char *data = read_file_all(path, &n);
    if (!data) return errf("cannot read %s: %s", path, strerror(errno));
    if (oj->slen == 0) {
        /* empty old_string on an empty file = set its content */
        if (n != 0) { free(data); return errf("old_string is empty; use write_file to replace a whole file"); }
    }
    int count = 0;
    if (oj->slen) for (const char *p = data; (p = strstr(p, oj->str)); p += oj->slen) count++;
    if (oj->slen && count == 0) {
        free(data);
        return errf("old_string not found in %s. Re-read the file and copy the text exactly (whitespace matters).", path);
    }
    if (count > 1 && !all) {
        free(data);
        return errf("old_string occurs %d times in %s; add surrounding context to make it unique, or set replace_all", count, path);
    }
    sbuf res = {0};
    if (!oj->slen) sb_add(&res, nj->str, nj->slen);
    else {
        const char *p = data, *q;
        while ((q = strstr(p, oj->str))) {
            sb_add(&res, p, (size_t)(q - p));
            sb_add(&res, nj->str, nj->slen);
            p = q + oj->slen;
            if (!all) break;
        }
        sb_adds(&res, p);
    }
    print_diff(data, sb_str(&res));
    if (count > 1) printf("%s  (%d replacements)%s\n", C_DIM, count, C_RESET);
    free(data);
    char *reason;
    if (ask_approval(&always_write, "this edit", &reason) != ASK_YES) { sb_free(&res); return denied(reason); }
    if (write_file_all(path, sb_str(&res), res.n) != 0) { sb_free(&res); return errf("writing %s failed: %s", path, strerror(errno)); }
    sb_free(&res);
    printf("%s  edited%s\n", C_DIM, C_RESET);
    sbuf r = {0};
    sb_printf(&r, "Edited %s (%d replacement%s)", path, count ? count : 1, count > 1 ? "s" : "");
    return sb_take(&r);
}

/* ---------- bash ---------- */

static char *t_bash(const json *a)
{
    const char *cmd = json_gets(a, "command");
    if (!cmd || !*cmd) return errf("command is required");
    int timeout = (int)json_getn(a, "timeout", g_cfg.bash_timeout);
    if (timeout < 1) timeout = g_cfg.bash_timeout;
    if (timeout > g_cfg.bash_timeout_max) timeout = g_cfg.bash_timeout_max;
    printf("%s● bash%s %s(timeout %ds)%s\n", C_MAGENTA, C_RESET, C_DIM, timeout, C_RESET);
    print_wrapped(cmd, "  $ ", "    ", C_BOLD, g_cfg.bash_command_lines);
    char *reason;
    if (ask_approval(&always_bash, "this command", &reason) != ASK_YES) return denied(reason);

    int pfd[2];
    if (pipe(pfd) != 0) return errf("pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return errf("fork: %s", strerror(errno)); }
    if (pid == 0) {
        setpgid(0, 0);                 /* own group: Ctrl-C goes to us, we kill the group */
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pfd[1]);

    sbuf out = {0};
    size_t dropped = 0;
    double start = now_s(), kill_time = 0;
    int killed = 0, at_bol = 1, shown_lines = 0;
    const int max_shown = g_cfg.bash_output_lines > 0 ? g_cfg.bash_output_lines : INT_MAX;
    tfilter tf = {0};
    printf("%s", C_DIM);
    for (;;) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        int pr = poll(&p, 1, 250);
        if (g_interrupted && !killed) { killpg(pid, SIGTERM); killed = 2; kill_time = now_s(); }
        if (!killed && now_s() - start >= timeout) { killpg(pid, SIGTERM); killed = 1; kill_time = now_s(); }
        if (killed && kill_time > 0 && now_s() - kill_time >= 3) { killpg(pid, SIGKILL); kill_time = 0; }
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        char buf[4096];
        ssize_t k = read(pfd[0], buf, sizeof buf);
        if (k < 0) { if (errno == EINTR) continue; break; }
        if (k == 0) break;
        if (out.n < OUT_MAX_BYTES) {
            size_t take = (size_t)k;
            if (out.n + take > OUT_MAX_BYTES) { dropped += out.n + take - OUT_MAX_BYTES; take = OUT_MAX_BYTES - out.n; }
            sb_add(&out, buf, take);
        } else {
            dropped += (size_t)k;
        }
        /* live echo (first lines only, to keep the phone screen readable) */
        for (ssize_t i = 0; i < k && shown_lines < max_shown; i++) {
            if (!tf_char(&tf, (unsigned char)buf[i])) continue;   /* no \r / escapes on screen */
            if (at_bol) { fputs("  │ ", stdout); at_bol = 0; }
            fputc(buf[i], stdout);
            if (buf[i] == '\n') { at_bol = 1; shown_lines++; }
        }
        fflush(stdout);
    }
    close(pfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!at_bol) fputc('\n', stdout);
    if (shown_lines >= max_shown) printf("  │ … (rest of the output went to the model; bash_output_lines)\n");
    printf("%s", C_RESET);

    sbuf r = {0};
    if (dropped) {
        /* keep the tail too: errors usually come last */
        sb_add(&r, out.p, out.n / 2);
        sb_printf(&r, "\n[... %zu bytes omitted ...]\n", dropped + out.n / 2);
        sb_add(&r, out.p + out.n / 2, out.n - out.n / 2);
    } else {
        sb_add(&r, sb_str(&out), out.n);
    }
    sb_free(&out);
    if (r.n && r.p[r.n - 1] != '\n') sb_addc(&r, '\n');
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (killed == 1) {
        sb_printf(&r, "[killed after %d s timeout. This phone is slow: try a quicker or narrower command, "
                      "or pass a larger timeout if it really needs longer]\n", timeout);
        printf("%s  killed: timeout after %ds%s\n", C_YELLOW, timeout, C_RESET);
    }
    else if (killed == 2) sb_adds(&r, "[interrupted by the user (Ctrl-C)]\n");
    else if (WIFSIGNALED(status)) sb_printf(&r, "[terminated by signal %d]\n", WTERMSIG(status));
    sb_printf(&r, "exit code: %d", code);
    printf("%s  exit %d%s\n", code == 0 ? C_DIM : C_RED, code, C_RESET);
    return sb_take(&r);
}

/* ---------- dispatch ---------- */

char *tool_run(const char *name, const char *args_json)
{
    const char *perr = NULL;
    json *a = json_parse(args_json, strlen(args_json), &perr);
    if (!a || a->t != J_OBJ) {
        json_free(a);
        printf("%s● %s: invalid arguments from model%s\n", C_RED, name, C_RESET);
        return errf("arguments are not a valid JSON object (%s). Retry with valid JSON.", perr ? perr : "not an object");
    }
    char *r;
    if (!strcmp(name, "read_file")) r = t_read_file(a);
    else if (!strcmp(name, "list_dir")) r = t_list_dir(a);
    else if (!strcmp(name, "glob")) r = t_glob(a);
    else if (!strcmp(name, "grep")) r = t_grep(a);
    else if (!strcmp(name, "write_file")) r = t_write_file(a);
    else if (!strcmp(name, "edit_file")) r = t_edit_file(a);
    else if (!strcmp(name, "bash")) r = t_bash(a);
    else {
        printf("%s● unknown tool %s%s\n", C_RED, name, C_RESET);
        r = errf("unknown tool '%s'", name);
    }
    json_free(a);
    if (!strncmp(r, "Error: ", 7)) printf("%s  %.*s%s\n", C_RED, term_cols() - 4 > 20 ? term_cols() - 4 : 20, r, C_RESET);
    return r;
}
