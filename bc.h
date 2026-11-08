/* berrycode - shared declarations */
#ifndef BC_H
#define BC_H

#include <signal.h>
#include "json.h"
#include "util.h"

#define BC_VERSION "0.1.2"

typedef struct {
    char  *base_url;     /* e.g. https://api.openai.com/v1 */
    char  *api_key;
    char  *model;
    char  *ca_bundle;    /* PEM file for TLS verification (NULL = libcurl default) */
    int    insecure;     /* skip TLS verification */
    int    yolo;         /* auto-approve writes + bash */
    int    stream;       /* use SSE streaming */
    int    color;
    int    max_tokens;   /* 0 = don't send */
    double temperature;  /* <0 = don't send */
    int    max_steps;    /* tool-call rounds per user message */
    int    timeout;      /* seconds without data before a request is abandoned */
    int    show_thinking;  /* print reasoning text (default: hidden, just "thinking...") */
    int    bash_timeout;   /* default seconds before a bash command is killed */
    int    bash_timeout_max; /* most the model may ask for */
    int    search_timeout; /* seconds before glob/grep give up */
    int    bash_command_lines; /* screen lines of a command shown before approval (0 = all) */
    int    bash_output_lines;  /* lines of command output echoed live (0 = all) */
    int    line_editor;    /* own prompt editing (raw mode); 0 = terminal's canonical mode */
    int    animation;      /* purple spinner while the model works */
    int    sync_output;    /* bracket block output with DEC 2026 so Term49CX paints whole frames */
    char  *data_dir;     /* sessions etc. */
    char  *exe_dir;
    char  *extra_headers;/* "Name: value" lines separated by '|' */
    int    opencode_session; /* send x-opencode-session header (stable ID per conversation) */
} config;

extern config g_cfg;
extern char *g_session_id;   /* stable ID for the active conversation (NULL = none yet) */
extern volatile sig_atomic_t g_interrupted;
extern volatile sig_atomic_t g_winch_soft;   /* SIGWINCH arrived (resize); main loop/lineedit consume it */

/* ---- terminal output ---- */
#define C_RESET  (g_cfg.color ? "\033[0m"  : "")
#define C_DIM    (g_cfg.color ? "\033[2m"  : "")
#define C_BOLD   (g_cfg.color ? "\033[1m"  : "")
#define C_RED    (g_cfg.color ? "\033[31m" : "")
#define C_GREEN  (g_cfg.color ? "\033[32m" : "")
#define C_YELLOW (g_cfg.color ? "\033[33m" : "")
#define C_BLUE   (g_cfg.color ? "\033[34m" : "")
#define C_MAGENTA (g_cfg.color ? "\033[35m" : "")
#define C_CYAN   (g_cfg.color ? "\033[36m" : "")
int term_cols(void);
int term_rows(void);       /* terminal rows (TIOCGWINSZ, COLUMNS/LINES fallback); 24 if unknown */
/* DEC 2026 synchronized output around a block (no-op unless tty + sync_output) */
void sync_begin(void);
void sync_end(void);
/* before a prompt: make sure the cursor sits above the status bar */
void prompt_guard(void);
/* fullscreen (alternate-screen) status bar; no-ops unless on a tty */
void fs_enter(void);
void fs_leave(void);
void fs_repaint(void);

/* ---- api.c ---- */
typedef struct {
    char *content;       /* assistant text (malloc'd, may be "") */
    json *tool_calls;    /* J_ARR in OpenAI message format, or NULL */
    char *finish_reason;
    char *error;         /* non-NULL on failure */
    long  http_status;
    long  prompt_tokens, completion_tokens;
    int   cancelled;
} chat_result;

void api_global_init(void);
int  api_chat(json *messages, const json *tools, chat_result *out);
void chat_result_free(chat_result *r);
char *api_get(const char *path, long *status, char **err);   /* GET base_url+path */

/* ---- spinner.c: purple "Munching..." animation while the model works ---- */
void spinner_start(void);   /* no-op if stdout isn't a terminal */
void spinner_stop(void);    /* stops and erases it */
int  spinner_active(void);

/* ---- lineedit.c: prompt with working backspace across wrapped lines + history ---- */
char *prompt_line(const char *prompt, int prompt_cols, int use_history, int *interrupted);

/* ---- tools.c ---- */
json *tools_schema(void);            /* OpenAI "tools" array */
/* run a tool call; returns malloc'd result text for the model */
char *tool_run(const char *name, const char *args_json);
void  tools_reset_session(void);     /* forget "always allow" choices */

#endif
