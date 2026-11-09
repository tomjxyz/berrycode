/* berrycode - a small opencode-style coding agent for the terminal (BlackBerry 10 / Term49)
 *
 * Talks to any OpenAI-compatible /chat/completions endpoint, lets the model read,
 * search, edit files and run shell commands. Writes and commands need approval
 * unless --yolo is given.
 */
#include "bc.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "terminal_io.h"

config g_cfg;
char *g_session_id = NULL;
volatile sig_atomic_t g_interrupted;
volatile sig_atomic_t g_winch_soft;

static json *g_msgs;         /* active conversation; in code mode [0] is the system prompt */
static json *g_other;        /* the inactive conversation (code <-> chat) */
static int   g_chat;         /* chat mode: no tools, no system prompt, nothing extra in context */
static json *g_tools;
static long  g_tok_in, g_tok_out;
static long  g_other_tok_in, g_other_tok_out;

/* ---------- small utilities ---------- */

int term_cols(void)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 1) return ws.ws_col;
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 10) return atoi(c);
    return 48;              /* Q20 narrow-screen fallback */
}

int term_rows(void)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1) return ws.ws_row;
    const char *c = getenv("LINES");
    if (c && atoi(c) > 5) return atoi(c);
    return 24;
}

static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }
static void on_sigwinch(int sig) { (void)sig; g_winch_soft = 1; }

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;      /* no SA_RESTART: blocking reads return EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    struct sigaction sw;
    memset(&sw, 0, sizeof sw);
    sw.sa_handler = on_sigwinch;    /* resize: lineedit/main consume g_winch_soft */
    sigemptyset(&sw.sa_mask);
    sigaction(SIGWINCH, &sw, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static int file_exists(const char *p) { struct stat st; return p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static char *path_join(const char *a, const char *b)
{
    sbuf s = {0};
    sb_printf(&s, "%s/%s", a, b);
    return sb_take(&s);
}

static void set_str(char **dst, const char *v) { free(*dst); *dst = v ? xstrdup(v) : NULL; }

static int parse_bool(const char *v)
{
    return v && (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

static char *find_exe_dir(const char *argv0)
{
    char buf[PATH_MAX];
    /* QNX: /proc/self/exefile holds the executable path; Linux: /proc/self/exe link */
    char *p = read_file_all("/proc/self/exefile", NULL);
    if (p && *p) {
        p[strcspn(p, "\n")] = 0;
        char *s = strrchr(p, '/');
        if (s) { *s = 0; return p; }
    }
    free(p);
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        char *s = strrchr(buf, '/');
        if (s) { *s = 0; return xstrdup(buf); }
    }
    if (strchr(argv0, '/') && realpath(argv0, buf)) {
        char *s = strrchr(buf, '/');
        if (s) { *s = 0; return xstrdup(buf); }
    }
    return xstrdup(".");
}

/* ---------- config ---------- */

static void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.base_url = xstrdup("https://api.openai.com/v1");
    g_cfg.model = xstrdup("gpt-4o-mini");
    g_cfg.stream = 1;
    g_cfg.color = 1;
    g_cfg.temperature = -1;
    g_cfg.max_steps = 40;
    g_cfg.timeout = 300;
    g_cfg.bash_timeout = 60;
    g_cfg.bash_timeout_max = 600;
    g_cfg.search_timeout = 15;
    g_cfg.bash_command_lines = 40;
    g_cfg.bash_output_lines = 40;
    g_cfg.line_editor = 1;
    g_cfg.animation = 1;
    g_cfg.sync_output = 1;
    g_cfg.opencode_session = 1;
}

static void config_set(const char *k, const char *v, const char *src, int line)
{
    if (!strcmp(k, "base_url")) set_str(&g_cfg.base_url, v);
    else if (!strcmp(k, "api_key")) set_str(&g_cfg.api_key, v);
    else if (!strcmp(k, "model")) set_str(&g_cfg.model, v);
    else if (!strcmp(k, "ca_bundle")) set_str(&g_cfg.ca_bundle, *v ? v : NULL);
    else if (!strcmp(k, "headers")) set_str(&g_cfg.extra_headers, *v ? v : NULL);
    else if (!strcmp(k, "insecure")) g_cfg.insecure = parse_bool(v);
    else if (!strcmp(k, "yolo")) g_cfg.yolo = parse_bool(v);
    else if (!strcmp(k, "stream")) g_cfg.stream = parse_bool(v);
    else if (!strcmp(k, "color")) g_cfg.color = parse_bool(v);
    else if (!strcmp(k, "max_tokens")) g_cfg.max_tokens = atoi(v);
    else if (!strcmp(k, "temperature")) g_cfg.temperature = *v ? atof(v) : -1;
    else if (!strcmp(k, "max_steps")) g_cfg.max_steps = atoi(v) > 0 ? atoi(v) : 40;
    else if (!strcmp(k, "timeout")) g_cfg.timeout = atoi(v) > 0 ? atoi(v) : 300;
    else if (!strcmp(k, "data_dir")) set_str(&g_cfg.data_dir, v);
    else if (!strcmp(k, "show_thinking")) g_cfg.show_thinking = parse_bool(v);
    else if (!strcmp(k, "bash_timeout")) g_cfg.bash_timeout = atoi(v) > 0 ? atoi(v) : 60;
    else if (!strcmp(k, "bash_timeout_max")) g_cfg.bash_timeout_max = atoi(v) > 0 ? atoi(v) : 600;
    else if (!strcmp(k, "search_timeout")) g_cfg.search_timeout = atoi(v) > 0 ? atoi(v) : 15;
    else if (!strcmp(k, "bash_command_lines")) g_cfg.bash_command_lines = atoi(v) > 0 ? atoi(v) : 0;
    else if (!strcmp(k, "line_editor")) g_cfg.line_editor = parse_bool(v);
    else if (!strcmp(k, "animation")) g_cfg.animation = parse_bool(v);
    else if (!strcmp(k, "sync_output")) g_cfg.sync_output = parse_bool(v);
    else if (!strcmp(k, "opencode_session")) g_cfg.opencode_session = parse_bool(v);
    else if (!strcmp(k, "bash_output_lines")) g_cfg.bash_output_lines = atoi(v) > 0 ? atoi(v) : 0;
    else fprintf(stderr, "berrycode: %s:%d: unknown setting '%s' (ignored)\n", src, line, k);
}

static int config_load(const char *path)
{
    char *data = read_file_all(path, NULL);
    if (!data) return -1;
    int ln = 0;
    char *save = NULL;
    for (char *l = strtok_r(data, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        ln++;
        while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == '#' || *l == ';') continue;
        char *eq = strchr(l, '=');
        if (!eq) { fprintf(stderr, "berrycode: %s:%d: expected key = value\n", path, ln); continue; }
        char *k = l, *v = eq + 1, *e = eq;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *h = v; *h; h++)        /* inline comment: whitespace then # */
            if (*h == '#' && h > v && (h[-1] == ' ' || h[-1] == '\t')) { *h = 0; break; }
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) v[--vl] = 0;
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) { v[vl - 1] = 0; v++; }
        config_set(k, v, path, ln);
    }
    free(data);
    return 0;
}

/* ---------- system prompt ---------- */

static char *build_system_prompt(void)
{
    sbuf s = {0};
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    time_t now = time(NULL);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    sb_printf(&s,
        "You are berrycode, an interactive coding agent running in a terminal. "
        "You help the user with software tasks: reading and explaining code, fixing bugs, writing files, running commands.\n\n"
        "Environment:\n"
        "- Device: BlackBerry 10 phone, QNX Neutrino 8 (ARMv7). The terminal app is Term49 with a small screen (~%d columns).\n"
        "- Working directory: %s\n"
        "- Date: %s\n"
        "- Shell for the bash tool: /bin/sh (QNX ksh). Many GNU tools and options are missing; "
        "there may be no git, python, curl or compiler. Check with `which` before relying on a tool.\n\n"
        "Guidelines:\n"
        "- Be concise: short answers, no wide tables or long banners; the screen is narrow.\n"
        "- Use read_file, list_dir, glob and grep to look around instead of bash where possible.\n"
        "- Before editing, read the relevant part of the file. Use edit_file for targeted changes "
        "(old_string must match exactly) and write_file for new files.\n"
        "- The user approves writes and commands; if they deny one, respect that and adapt.\n"
        "- Do the task with tools rather than describing what you would do. When finished, give a brief summary.\n",
        term_cols(), cwd, date);
    static const char *ctx_files[] = { "AGENTS.md", "BERRYCODE.md", "CLAUDE.md" };
    for (size_t i = 0; i < sizeof ctx_files / sizeof *ctx_files; i++) {
        size_t n;
        char *d = read_file_all(ctx_files[i], &n);
        if (!d) continue;
        if (n > 16384) { d[16384] = 0; }
        sb_printf(&s, "\nProject instructions from %s:\n%s\n", ctx_files[i], d);
        free(d);
        break;
    }
    return sb_take(&s);
}

static void refresh_system(void)
{
    if (g_chat) return;          /* chat mode sends only what the user and model said */
    char *sp = build_system_prompt();
    json *m = json_obj();
    json_set(m, "role", json_str("system"));
    json_set(m, "content", json_str(sp));
    free(sp);
    if (json_len(g_msgs) > 0 && !strcmp(json_gets(json_at(g_msgs, 0), "role") ? json_gets(json_at(g_msgs, 0), "role") : "", "system")) {
        json_free(g_msgs->items[0]);
        g_msgs->items[0] = m;
    } else {
        json_push(g_msgs, m);
        /* move to front */
        for (int i = g_msgs->len - 1; i > 0; i--) g_msgs->items[i] = g_msgs->items[i - 1];
        g_msgs->items[0] = m;
    }
}

static void reset_conversation(void);
static char *other_sid = NULL;   /* the inactive mode's conversation ID (code <-> chat) */

/* random 8-4-4-4-12 hex ID (uuid-shaped, no OS uuid lib on BB10) */
static char *new_sid(void)
{
    static const char *h = "0123456789abcdef";
    char b[37];
    int pos = 0;
    /* mix time+pid into rand() so two quick restarts don't repeat */
    srand((unsigned)time(NULL) ^ (unsigned)getpid() ^ (unsigned)rand());
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) b[pos++] = '-';
        else b[pos++] = h[rand() % 16];
    }
    b[36] = 0;
    return xstrdup(b);
}

static void reset_conversation(void)
{
    json_free(g_msgs);
    g_msgs = json_arr();
    refresh_system();
    g_tok_in = g_tok_out = 0;
    fs_set_tokens(g_tok_in, g_tok_out);
    free(g_session_id);
    g_session_id = new_sid();
    if (!g_chat) tools_reset_session();
}

/* ---------- sessions ---------- */

static char *session_path(void)
{
    if (!g_cfg.data_dir) return NULL;
    return path_join(g_cfg.data_dir, g_chat ? "chat.json" : "session.json");
}

static void session_save(void)
{
    char *p = session_path();
    if (!p) return;
    if (mkdir_p(g_cfg.data_dir) == 0) {
        json *s = json_obj();
        char cwd[PATH_MAX];
        json_set(s, "cwd", json_str(getcwd(cwd, sizeof cwd) ? cwd : "."));
        json_set(s, "model", json_str(g_cfg.model));
        json_set(s, "tokens_in", json_num((double)g_tok_in));
        json_set(s, "tokens_out", json_num((double)g_tok_out));
        if (g_session_id) json_set(s, "session_id", json_str(g_session_id));
        json *msgs = json_arr();
        for (int i = g_chat ? 0 : 1; i < json_len(g_msgs); i++) json_push(msgs, json_clone(json_at(g_msgs, i)));
        json_set(s, "messages", msgs);
        char *d = json_dump(s);
        if (write_file_all(p, d, strlen(d)) != 0)
            fprintf(stderr, "berrycode: could not save session to %s: %s\n", p, strerror(errno));
        free(d);
        json_free(s);
    }
    free(p);
}

static long saved_tokens(const json *session, const char *key)
{
    double count = json_getn(session, key, 0);
    if (!(count > 0)) return 0;
    if (count >= (double)LONG_MAX) return LONG_MAX;
    return (long)count;
}

static int session_load(int replay)
{
    char *p = session_path();
    if (!p) return -1;
    size_t n;
    char *d = read_file_all(p, &n);
    if (!d) { printf("%sno saved session at %s%s\n", C_DIM, p, C_RESET); free(p); return -1; }
    json *s = json_parse(d, n, NULL);
    free(d);
    json *msgs = json_get(s, "messages");
    if (!msgs || msgs->t != J_ARR) { printf("%ssaved session is unreadable: %s%s\n", C_RED, p, C_RESET); json_free(s); free(p); return -1; }
    const char *cwd = json_gets(s, "cwd");
    if (cwd && !g_chat && chdir(cwd) != 0) printf("%s(could not cd to %s)%s\n", C_DIM, cwd, C_RESET);
    reset_conversation();
    for (int i = 0; i < json_len(msgs); i++) json_push(g_msgs, json_clone(json_at(msgs, i)));
    refresh_system();
    const char *sid = json_gets(s, "session_id");
    if (sid && *sid) { free(g_session_id); g_session_id = xstrdup(sid); }
    g_tok_in = saved_tokens(s, "tokens_in");
    g_tok_out = saved_tokens(s, "tokens_out");
    fs_set_tokens(g_tok_in, g_tok_out);
    if (replay) history_replay(msgs, g_chat);
    json_free(s);
    free(p);
    return 0;
}

/* ---------- agent loop ---------- */

static void push_msg(const char *role, const char *content)
{
    json *m = json_obj();
    json_set(m, "role", json_str(role));
    json_set(m, "content", json_str(content));
    json_push(g_msgs, m);
}

static void push_tool_result(const char *id, const char *content)
{
    json *m = json_obj();
    json_set(m, "role", json_str("tool"));
    json_set(m, "tool_call_id", json_str(id));
    json_set(m, "content", json_str(content));
    json_push(g_msgs, m);
}

static void run_turn(const char *user_text)
{
    push_msg("user", user_text);
    refresh_system();
    g_interrupted = 0;
    int step;
    for (step = 0; step < g_cfg.max_steps; step++) {
        printf("%s", C_RESET);
        fflush(stdout);
        chat_result r;
        api_chat(g_msgs, g_chat ? NULL : g_tools, &r);
        if (r.prompt_tokens > 0)
            g_tok_in = g_tok_in > LONG_MAX - r.prompt_tokens ? LONG_MAX : g_tok_in + r.prompt_tokens;
        if (r.completion_tokens > 0)
            g_tok_out = g_tok_out > LONG_MAX - r.completion_tokens ? LONG_MAX : g_tok_out + r.completion_tokens;
        fs_set_tokens(g_tok_in, g_tok_out);
        if (r.error) {
            if (r.cancelled) printf("%s[cancelled]%s\n", C_YELLOW, C_RESET);
            else printf("%serror: %s%s\n", C_RED, r.error, C_RESET);
            chat_result_free(&r);
            break;
        }
        json *m = json_obj();
        json_set(m, "role", json_str("assistant"));
        json_set(m, "content", json_str(r.content));
        if (r.tool_calls && g_chat)       /* chat mode never runs tools, whatever the server sends */
            printf("%s[model tried to call a tool; ignored in chat mode]%s\n", C_DIM, C_RESET);
        else if (r.tool_calls) { json_set(m, "tool_calls", r.tool_calls); r.tool_calls = NULL; }
        json_push(g_msgs, m);
        json *tcs = json_get(m, "tool_calls");
        if (r.finish_reason && !strcmp(r.finish_reason, "length"))
            printf("%s[response cut off: max_tokens reached]%s\n", C_YELLOW, C_RESET);
        chat_result_free(&r);
        if (!tcs) break;

        for (int i = 0; i < json_len(tcs); i++) {
            json *tc = json_at(tcs, i);
            const char *id = json_gets(tc, "id");
            json *fn = json_get(tc, "function");
            const char *name = json_gets(fn, "name");
            const char *args = json_gets(fn, "arguments");
            if (g_interrupted) { push_tool_result(id, "Cancelled: the user interrupted before this ran."); continue; }
            char *res = tool_run(name ? name : "?", args ? args : "{}");
            push_tool_result(id, res);
            free(res);
        }
        if (g_interrupted) { printf("%s[interrupted]%s\n", C_YELLOW, C_RESET); break; }
    }
    if (step >= g_cfg.max_steps)
        printf("%s[stopped after %d tool rounds; say \"continue\" to keep going]%s\n", C_YELLOW, g_cfg.max_steps, C_RESET);
    g_interrupted = 0;
    session_save();
}

/* ---------- slash commands ---------- */

static void print_status(void)
{
    char cwd[PATH_MAX];
    printf("%sberrycode %s%s\n", C_BOLD, BC_VERSION, C_RESET);
    printf("  model:    %s\n", g_cfg.model);
    printf("  endpoint: %s\n", g_cfg.base_url);
    printf("  api key:  %s\n", g_cfg.api_key && *g_cfg.api_key ? "set" : "(none)");
    printf("  mode:     %s\n", g_chat ? "chat (no tools, no system prompt)" : "code (tools on)");
    printf("  approve:  %s\n", g_cfg.yolo ? "YOLO - everything runs without asking" : "ask for writes + commands (reads are free)");
    printf("  tls:      %s\n", g_cfg.insecure ? "NOT verified (insecure)" : g_cfg.ca_bundle ? g_cfg.ca_bundle : "libcurl default CAs");
    printf("  thinking: %s\n", g_cfg.show_thinking ? "shown" : "hidden");
    printf("  timeouts: bash %ds (max %ds), glob/grep %ds\n", g_cfg.bash_timeout, g_cfg.bash_timeout_max, g_cfg.search_timeout);
    char cl[16] = "all", ol[16] = "all";
    if (g_cfg.bash_command_lines) snprintf(cl, sizeof cl, "%d", g_cfg.bash_command_lines);
    if (g_cfg.bash_output_lines) snprintf(ol, sizeof ol, "%d", g_cfg.bash_output_lines);
    printf("  bash view: command %s lines, output %s lines\n", cl, ol);
    printf("  screen:   fullscreen (alt screen + status bar), sync %s (%dx%d)\n",
           g_cfg.sync_output ? "on (DEC 2026)" : "off", term_cols(), term_rows());
    printf("  cwd:      %s\n", getcwd(cwd, sizeof cwd) ? cwd : "?");
    printf("  messages: %d, tokens %ld in / %ld out\n", json_len(g_msgs) - (g_chat ? 0 : 1), g_tok_in, g_tok_out);
    printf("  opencode_session: %s%s%s\n", g_cfg.opencode_session ? "on (" : "off",
           g_cfg.opencode_session && g_session_id ? g_session_id : "", g_cfg.opencode_session ? ")" : "");
    if (g_cfg.data_dir) printf("  data:     %s\n", g_cfg.data_dir);
}

static void print_help(void)
{
    printf("Type a request and press Enter. End a line with \\ to continue it,\n"
           "or put \"\"\" on its own line to start/end a multi-line block.\n"
           "Ctrl-C cancels the current reply/command; Ctrl-D or /quit exits.\n"
           "Paste (Term49CX bracketed paste) is inserted as one block; pasted\n"
           "newlines become spaces. Press Meta+i in Term49CX for CJK input.\n\n"
           "  /help            this help\n"
           "  /status          model, endpoint, approval mode\n"
           "  /model [name]    show or switch model\n"
           "  /models          list models from the endpoint\n"
           "  /yolo [on|off]   toggle auto-approval of writes + commands\n"
           "  /chat            chat mode: plain conversation, no tools\n"
           "  /code            back to code mode (tools)\n"
           "  /thinking        show/hide model reasoning (hidden by default)\n"
           "  /timeout [SECS]  show or set the default bash timeout\n"
           "  /set KEY VALUE   change a config setting for this session\n"
           "  /clear           start a new conversation\n"
           "  /resume          reload the last saved session\n"
           "  /cd DIR          change working directory\n"
           "  !command         run a shell command yourself (not sent to the model)\n"
           "  /quit            exit\n");
}

static void list_models(void)
{
    long st;
    char *err = NULL;
    char *body = api_get("/models", &st, &err);
    if (!body) { printf("%serror: %s%s\n", C_RED, err ? err : "?", C_RESET); free(err); return; }
    json *j = json_parse(body, strlen(body), NULL);
    json *data = json_get(j, "data");
    if (!data) data = json_get(j, "models");
    int n = json_len(data);
    for (int i = 0; i < n; i++) {
        json *m = json_at(data, i);
        const char *id = json_gets(m, "id");
        if (!id) id = json_gets(m, "name");
        if (id) printf("  %s%s\n", id, !strcmp(id, g_cfg.model) ? "  *" : "");
    }
    if (!n) printf("%s(no models in response)%s\n", C_DIM, C_RESET);
    json_free(j);
    free(body);
}

static void run_user_shell(const char *cmd)
{
    int pfd[2];
    if (pipe(pfd) != 0) { perror("pipe"); return; }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); perror("fork"); return; }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[4096];
    tfilter filter = {0};
    for (;;) {
        ssize_t n = read(pfd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sbuf clean = {0};
        for (ssize_t i = 0; i < n; i++)
            if (tf_char(&filter, (unsigned char)buf[i])) sb_addc(&clean, buf[i]);
        term_write(sb_str(&clean), clean.n);
        sb_free(&clean);
    }
    close(pfd[0]);
    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0 && errno == EINTR) {}
    if (rc != 0) printf("%s[exit %d]%s\n", C_DIM, WIFEXITED(rc) ? WEXITSTATUS(rc) : rc, C_RESET);
}

static void set_mode(int chat, int announce)
{
    if (chat != g_chat) {
        json *t = g_msgs;
        g_msgs = g_other;
        g_other = t;
        char *s = g_session_id;       /* each mode keeps its own stable ID */
        g_session_id = other_sid;
        other_sid = s;
        long tokens = g_tok_in;
        g_tok_in = g_other_tok_in;
        g_other_tok_in = tokens;
        tokens = g_tok_out;
        g_tok_out = g_other_tok_out;
        g_other_tok_out = tokens;
        fs_set_tokens(g_tok_in, g_tok_out);
        g_chat = chat;
        if (!g_msgs) { g_msgs = json_arr(); refresh_system(); }
        else refresh_system();
        if (!g_session_id) g_session_id = new_sid();
    }
    if (!announce) return;
    if (g_chat) printf("%schat mode: just you and the model - no tools, no system prompt.\n"
                       "/code switches back (each mode keeps its own conversation)%s\n", C_MAGENTA, C_RESET);
    else printf("%scode mode: the model can use tools (writes + commands ask first)%s\n", C_CYAN, C_RESET);
}

/* Bramble, the berrycode mascot: a purple berry with eyes and little legs */
static void print_mascot(void)
{
    const char *P = g_cfg.color ? "\033[1;35m" : "";   /* berry purple */
    const char *E = g_cfg.color ? "\033[1;37m" : "";   /* eye whites */
    const char *G = g_cfg.color ? "\033[32m" : "";     /* stem */
    const char *L = g_cfg.color ? "\033[35m" : "";     /* legs */
    const char *R = C_RESET, *B = C_BOLD, *D = C_DIM;
    /* text beside the berry only if it fits (never touching the last column) */
    int cols = term_cols();
    const char *mode = g_chat ? "chat mode" : "code mode";
    int side = cols - 1 >= 17 + (int)strlen("berrycode " BC_VERSION) &&
               cols - 1 >= 17 + (int)strlen(g_cfg.model);
    char model[64];
    snprintf(model, sizeof model, "%.*s", cols - 2 > 0 && cols - 2 < 63 ? cols - 2 : 63, g_cfg.model);
    printf("      %s\\|/%s\n", G, R);
    if (side) {
        printf("   %s.-@@@@@-.%s     %sberrycode%s %s\n", P, R, B, R, BC_VERSION);
        printf("  %s(@@ %so o%s @@)%s    %s%s%s\n", P, E, P, R, D, model, R);
        printf("  %s(@@@ %sv%s @@@)%s    %s%s%s\n", P, E, P, R, D, mode, R);
    } else {
        printf("   %s.-@@@@@-.%s\n", P, R);
        printf("  %s(@@ %so o%s @@)%s\n", P, E, P, R);
        printf("  %s(@@@ %sv%s @@@)%s\n", P, E, P, R);
    }
    printf("   %s'-@@@@@-'%s\n", P, R);
    printf("     %s/   \\%s\n", L, R);
    printf("   %s_/     \\_%s\n", L, R);
    if (!side) printf("%sberrycode%s %s\n%s%s - %s%s\n", B, R, BC_VERSION, D, model, mode, R);
}

/* returns 1 to quit */
static int slash_command(char *line)
{
    char *arg = line + strcspn(line, " \t");
    if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
    if (!strcmp(line, "/quit") || !strcmp(line, "/exit") || !strcmp(line, "/q")) return 1;
    else if (!strcmp(line, "/help") || !strcmp(line, "/?")) print_help();
    else if (!strcmp(line, "/status")) print_status();
    else if (!strcmp(line, "/clear") || !strcmp(line, "/new")) { reset_conversation(); printf("%snew conversation%s\n", C_DIM, C_RESET); }
    else if (!strcmp(line, "/resume")) session_load(1);
    else if (!strcmp(line, "/chat")) set_mode(1, 1);
    else if (!strcmp(line, "/set")) {
        char *val = arg + strcspn(arg, " \t=");
        if (*val) { *val++ = 0; while (*val == ' ' || *val == '\t' || *val == '=') val++; }
        if (!*arg || !*val) printf("usage: /set KEY VALUE   (any config setting, e.g. /set bash_output_lines 100)\n");
        else if (!strcmp(arg, "api_key") || !strcmp(arg, "data_dir")) printf("%sset %s in the config file%s\n", C_DIM, arg, C_RESET);
        else {
            config_set(arg, val, "/set", 0);
            json_free(g_tools);
            g_tools = tools_schema();
            printf("%s%s = %s (this session only; put it in the config to keep it)%s\n", C_DIM, arg, val, C_RESET);
        }
    }
    else if (!strcmp(line, "/thinking")) {
        if (!strcmp(arg, "on")) g_cfg.show_thinking = 1;
        else if (!strcmp(arg, "off")) g_cfg.show_thinking = 0;
        else g_cfg.show_thinking = !g_cfg.show_thinking;
        printf("%sthinking %s%s\n", C_DIM, g_cfg.show_thinking ? "shown" : "hidden", C_RESET);
    } else if (!strcmp(line, "/timeout")) {
        if (*arg && atoi(arg) > 0) g_cfg.bash_timeout = atoi(arg);
        if (g_cfg.bash_timeout_max < g_cfg.bash_timeout) g_cfg.bash_timeout_max = g_cfg.bash_timeout;
        json_free(g_tools);
        g_tools = tools_schema();          /* the tool descriptions mention the limits */
        printf("bash timeout: %ds (model may ask for up to %ds)\n", g_cfg.bash_timeout, g_cfg.bash_timeout_max);
    }
    else if (!strcmp(line, "/code")) set_mode(0, 1);
    else if (!strcmp(line, "/models")) list_models();
    else if (!strcmp(line, "/model")) {
        if (*arg) set_str(&g_cfg.model, arg);
        printf("model: %s\n", g_cfg.model);
    } else if (!strcmp(line, "/yolo")) {
        if (!strcmp(arg, "on")) g_cfg.yolo = 1;
        else if (!strcmp(arg, "off")) g_cfg.yolo = 0;
        else g_cfg.yolo = !g_cfg.yolo;
        printf("%s%s%s\n", g_cfg.yolo ? C_RED : C_GREEN,
               g_cfg.yolo ? "YOLO on: writes and commands run without asking" : "YOLO off: writes and commands need approval", C_RESET);
    } else if (!strcmp(line, "/cd")) {
        const char *d = *arg ? arg : getenv("HOME");
        if (!d || chdir(d) != 0) printf("%scd: %s%s\n", C_RED, strerror(errno), C_RESET);
        else { char cwd[PATH_MAX]; printf("%s\n", getcwd(cwd, sizeof cwd) ? cwd : d); refresh_system(); }
    } else printf("%sunknown command %s (try /help)%s\n", C_RED, line, C_RESET);
    return 0;
}

/* read a (possibly multi-line) message; NULL on EOF */
static char *read_input(void)
{
    sbuf b = {0};
    int block = 0, first = 1;
    for (;;) {
        fs_repaint();                            /* update model/directory in the footer */
        char pr[64];
        snprintf(pr, sizeof pr, "%s%s%s%s ", C_BOLD, g_chat ? C_MAGENTA : C_GREEN,
                 first ? ">" : "…", C_RESET);
        int intr;
        char *l = prompt_line(pr, 2, 1, &intr);
        if (!l) {
            if (intr) {
                printf("\n");
                g_interrupted = 0;
                if (b.n || block) { sb_free(&b); block = 0; first = 1; continue; }
                sb_free(&b);
                return xstrdup("\x03");      /* Ctrl-C on empty prompt */
            }
            if (b.n) return sb_take(&b);
            sb_free(&b);
            return NULL;
        }
        first = 0;
        if (!strcmp(l, "\"\"\"")) {
            free(l);
            if (block) return sb_take(&b);
            block = 1;
            continue;
        }
        size_t n = strlen(l);
        if (block) { sb_adds(&b, l); sb_addc(&b, '\n'); free(l); continue; }
        if (n && l[n - 1] == '\\') { l[n - 1] = 0; sb_adds(&b, l); sb_addc(&b, '\n'); free(l); continue; }
        sb_adds(&b, l);
        free(l);
        return sb_take(&b);
    }
}

static void usage(void)
{
    printf("usage: berrycode [options] [prompt...]\n\n"
           "  -m, --model NAME     model (default from config, else gpt-4o-mini)\n"
           "  -u, --base-url URL   OpenAI-compatible endpoint, e.g. http://192.168.1.5:11434/v1\n"
           "  -k, --api-key KEY    API key (or env OPENAI_API_KEY)\n"
           "  -y, --yolo           don't ask before writes and commands\n"
           "      --chat           start in chat mode (no tools, nothing added to context)\n"
           "  -c, --continue       resume the last session\n"
           "  -p, --print TEXT     run one request non-interactively and exit\n"
           "  -C, --dir DIR        start in DIR\n"
           "      --config FILE    config file (default ~/.berrycode/config)\n"
           "      --ca FILE        CA bundle (PEM) for HTTPS\n"
           "      --insecure       don't verify HTTPS certificates\n"
           "      --no-stream      wait for whole replies instead of streaming\n"
           "      --no-color       plain output\n"
           "  -V, --version\n\n"
           "Env: OPENAI_API_KEY, OPENAI_BASE_URL, BERRYCODE_MODEL, BERRYCODE_CONFIG, NO_COLOR\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    config_defaults();
    g_cfg.exe_dir = find_exe_dir(argv[0]);

    /* first pass: only --config / -h / -V */
    const char *cfg_file = getenv("BERRYCODE_CONFIG");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_file = argv[++i];
        else if (!strncmp(argv[i], "--config=", 9)) cfg_file = argv[i] + 9;
    }
    const char *home = getenv("HOME");
    if (home && *home) g_cfg.data_dir = path_join(home, ".berrycode");
    else g_cfg.data_dir = path_join(g_cfg.exe_dir, ".berrycode");

    /* config: next to the binary, then ~/.berrycode/config, then --config (later wins) */
    char *p1 = path_join(g_cfg.exe_dir, "berrycode.conf");
    config_load(p1);
    free(p1);
    char *p2 = path_join(g_cfg.data_dir, "config");
    config_load(p2);
    free(p2);
    if (cfg_file && config_load(cfg_file) != 0) {
        fprintf(stderr, "berrycode: cannot read config %s: %s\n", cfg_file, strerror(errno));
        return 1;
    }
    if (getenv("OPENAI_API_KEY") && *getenv("OPENAI_API_KEY")) set_str(&g_cfg.api_key, getenv("OPENAI_API_KEY"));
    if (getenv("OPENAI_BASE_URL") && *getenv("OPENAI_BASE_URL")) set_str(&g_cfg.base_url, getenv("OPENAI_BASE_URL"));
    if (getenv("BERRYCODE_MODEL") && *getenv("BERRYCODE_MODEL")) set_str(&g_cfg.model, getenv("BERRYCODE_MODEL"));
    if (getenv("NO_COLOR")) g_cfg.color = 0;

    static struct option opts[] = {
        { "model", required_argument, 0, 'm' }, { "base-url", required_argument, 0, 'u' },
        { "api-key", required_argument, 0, 'k' }, { "yolo", no_argument, 0, 'y' },
        { "continue", no_argument, 0, 'c' }, { "print", required_argument, 0, 'p' },
        { "dir", required_argument, 0, 'C' }, { "config", required_argument, 0, 1 },
        { "ca", required_argument, 0, 2 }, { "insecure", no_argument, 0, 3 },
        { "no-stream", no_argument, 0, 4 }, { "no-color", no_argument, 0, 5 },
        { "chat", no_argument, 0, 6 },
        { "help", no_argument, 0, 'h' }, { "version", no_argument, 0, 'V' }, { 0, 0, 0, 0 }
    };
    int resume = 0, start_chat = 0, opt;
    const char *oneshot = NULL;
    while ((opt = getopt_long(argc, argv, "m:u:k:ycp:C:hV", opts, NULL)) != -1) {
        switch (opt) {
        case 'm': set_str(&g_cfg.model, optarg); break;
        case 'u': set_str(&g_cfg.base_url, optarg); break;
        case 'k': set_str(&g_cfg.api_key, optarg); break;
        case 'y': g_cfg.yolo = 1; break;
        case 'c': resume = 1; break;
        case 'p': oneshot = optarg; break;
        case 'C':
            if (chdir(optarg) != 0) { fprintf(stderr, "berrycode: cd %s: %s\n", optarg, strerror(errno)); return 1; }
            break;
        case 1: break;
        case 2: set_str(&g_cfg.ca_bundle, optarg); break;
        case 3: g_cfg.insecure = 1; break;
        case 4: g_cfg.stream = 0; break;
        case 5: g_cfg.color = 0; break;
        case 6: start_chat = 1; break;
        case 'h': usage(); return 0;
        case 'V': printf("berrycode %s\n", BC_VERSION); return 0;
        default: usage(); return 1;
        }
    }
    if (!isatty(1)) g_cfg.color = 0;

    /* CA bundle: explicit, else cacert.pem beside the binary or in the data dir */
    if (!g_cfg.ca_bundle) {
        char *c1 = path_join(g_cfg.exe_dir, "cacert.pem"), *c2 = path_join(g_cfg.data_dir, "cacert.pem");
        if (file_exists(c1)) g_cfg.ca_bundle = c1, c1 = NULL;
        else if (file_exists(c2)) g_cfg.ca_bundle = c2, c2 = NULL;
        free(c1);
        free(c2);
    } else if (!file_exists(g_cfg.ca_bundle)) {
        fprintf(stderr, "berrycode: warning: ca_bundle %s not found\n", g_cfg.ca_bundle);
    }

    install_signals();
    api_global_init();
    g_tools = tools_schema();
    g_msgs = json_arr();
    reset_conversation();
    if (start_chat) set_mode(1, 0);

    /* remaining args form a prompt too: berrycode "fix the build" */
    sbuf extra = {0};
    for (int i = optind; i < argc; i++) { if (extra.n) sb_addc(&extra, ' '); sb_adds(&extra, argv[i]); }

    if (oneshot || (extra.n && !isatty(0))) {
        if (resume) session_load(0); /* --print keeps stdout free of replayed history */
        run_turn(oneshot ? oneshot : extra.p);
        sb_free(&extra);
        return 0;
    }

    fs_enter();                 /* alt screen + status bar (no-op unless tty) */
    sync_begin();               /* mascot + banner land as one Term49CX frame */
    print_mascot();
    printf("%s%s%s\n", C_DIM, g_cfg.base_url, C_RESET);
    if (g_chat) printf("%schat mode: no tools, nothing extra in context. /code for tools%s\n", C_MAGENTA, C_RESET);
    else if (g_cfg.yolo) printf("%sYOLO mode: writes and commands run without asking%s\n", C_RED, C_RESET);
    else printf("%swrites + commands ask first, reads don't.\n/chat for plain chat, /help for commands%s\n", C_DIM, C_RESET);
    if (!g_cfg.api_key && strncmp(g_cfg.base_url, "http://", 7) != 0)
        printf("%sno API key set: put api_key = ... in %s/config%s\n", C_YELLOW, g_cfg.data_dir, C_RESET);
    sync_end();                /* banner lands as one frame; turns stream live */
    if (resume) session_load(1);
    if (extra.n) run_turn(extra.p);
    sb_free(&extra);

    int ctrlc = 0;
    for (;;) {
        if (g_winch_soft) g_winch_soft = 0;   /* rows re-read at next paint */
        printf("\n");
        char *in = read_input();              /* guard + bar, per prompt line */
        if (g_winch_soft) g_winch_soft = 0;
        if (!in) { printf("\n"); break; }
        if (!strcmp(in, "\x03")) {
            free(in);
            if (ctrlc) break;
            ctrlc = 1;
            printf("%s(press Ctrl-C again, Ctrl-D or /quit to exit)%s\n", C_DIM, C_RESET);
            continue;
        }
        ctrlc = 0;
        char *t = in;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) { free(in); continue; }
        if (*t == '/' && !strchr(t, '\n')) {
            int q = slash_command(t);
            free(in);
            if (q) break;
            continue;
        }
        if (*t == '!') { run_user_shell(t + 1); free(in); continue; }
        run_turn(t);
        free(in);
    }
    session_save();
    fs_leave();
    return 0;
}
