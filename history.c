/* Replay saved messages as display-only output. Never dispatch tools or
 * change conversation data; treat persisted terminal controls as untrusted. */
#include "bc.h"

#include <stdlib.h>
#include <string.h>
#include "terminal_io.h"

enum { TEXT, MARKDOWN, PROMPT };

static void replay_text(const char *text, size_t n, int style)
{
    mfilter filter = {0};
    mdf render = {0};
    int newline = 0;
    for (size_t offset = 0; offset < n && !g_interrupted;) {
        size_t take = n - offset;
        if (take > 4096) take = 4096;
        sbuf plain = {0}, display = {0}, rich = {0};
        mf_add(&filter, text + offset, take, &plain, &display);
        if (style == MARKDOWN) md_add(&render, sb_str(&display), display.n, &rich);
        else if (style == PROMPT) {
            /* Saved user text must not cancel the input's bold style with
             * embedded SGR. Strip it and reapply bold for every chunk/part. */
            sb_adds(&rich, C_BOLD);
            sb_add(&rich, sb_str(&plain), plain.n);
        }
        else sb_add(&rich, sb_str(&display), display.n);
        if (rich.n) {
            fwrite(rich.p, 1, rich.n, stdout);
            newline = rich.p[rich.n - 1] == '\n';
        }
        sb_free(&plain);
        sb_free(&display);
        sb_free(&rich);
        offset += take;
    }
    if (style == MARKDOWN) {
        sbuf tail = {0};
        md_end(&render, &tail);
        if (tail.n) {
            fwrite(tail.p, 1, tail.n, stdout);
            /* md_end may finish with SGR rather than a printable newline.
             * An extra separator is preferable to a joined pair of messages. */
            newline = tail.p[tail.n - 1] == '\n';
        }
        sb_free(&tail);
    }
    mf_flush(&filter);
    printf("%s%s", C_RESET, newline ? "" : "\n");
}

static void content(const json *message, int style)
{
    const json *value = json_get(message, "content");
    if (value && value->t == J_STR) replay_text(value->str, value->slen, style);
    else if (value && value->t == J_ARR) {
        /* Accept OpenAI text-part arrays in imported/older sessions too. */
        for (int i = 0; i < json_len(value) && !g_interrupted; i++) {
            const json *text = json_get(json_at(value, i), "text");
            if (text && text->t == J_STR) replay_text(text->str, text->slen, style);
        }
    } else replay_text("", 0, TEXT);
}

void history_replay(const json *messages, int chat)
{
    for (int i = 0; i < json_len(messages) && !g_interrupted; i++) {
        const json *message = json_at(messages, i);
        const char *role = json_gets(message, "role");
        if (!role) continue;
        if (!strcmp(role, "user")) {
            printf("\n%s%s> %s", C_BOLD, chat ? C_MAGENTA : C_GREEN, C_RESET);
            content(message, PROMPT);
        } else if (!strcmp(role, "assistant")) {
            content(message, MARKDOWN);
            const json *calls = json_get(message, "tool_calls");
            for (int j = 0; j < json_len(calls) && !g_interrupted; j++) {
                const json *fn = json_get(json_at(calls, j), "function");
                const char *name = json_gets(fn, "name");
                const char *args = json_gets(fn, "arguments");
                printf("%s[tool call] %s", C_DIM, C_RESET);
                if (name) replay_text(name, strlen(name), TEXT);
                if (args) replay_text(args, strlen(args), TEXT);
            }
        } else if (!strcmp(role, "tool")) {
            printf("%s[tool result]%s\n", C_DIM, C_RESET);
            content(message, TEXT);
        }
    }
    if (g_interrupted) printf("%s[history display interrupted]%s\n", C_DIM, C_RESET);
    g_interrupted = 0;  /* Ctrl-C cancels display, not the restored conversation */
}
