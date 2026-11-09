/* Host-only driver: no network, SDK or phone required. */
#include "bc.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "terminal_io.h"

config g_cfg;
volatile sig_atomic_t g_winch_soft;
volatile sig_atomic_t g_interrupted;
static int height = 6, width = 24;
int term_rows(void) { return height; }
int term_cols(void) { return width; }

static void *writer(void *arg)
{
    const char *name = arg;
    for (int i = 0; i < 100; i++) printf("%s%03d\n", name, i);
    return NULL;
}

int main(int argc, char **argv)
{
    g_cfg.model = "test";
    g_cfg.color = 1;
    g_cfg.sync_output = 1;
    fs_enter();
    if (argc > 1 && !strcmp(argv[1], "tokens")) {
        fs_set_tokens(999, 1000);
        fs_set_tokens(1001, 999999);
        fs_set_tokens(1000000, 1000001);
        fs_set_tokens(12345, 5678901);
        g_cfg.model = "provider/a-very-long-model";
        fs_repaint();
    } else if (argc > 1 && (!strcmp(argv[1], "history") || !strcmp(argv[1], "history-chat"))) {
        json *messages = json_arr();
        json *system = json_obj();
        json_set(system, "role", json_str("system"));
        json_set(system, "content", json_str("SECRET_SYSTEM_PROMPT"));
        json_push(messages, system);
        for (int i = 0; i < 30; i++) {
            char text[64];
            json *user = json_obj(), *assistant = json_obj();
            json_set(user, "role", json_str("user"));
            snprintf(text, sizeof text, "user%02d", i);
            json_set(user, "content", json_str(text));
            json_push(messages, user);
            json_set(assistant, "role", json_str("assistant"));
            snprintf(text, sizeof text, "**reply%02d**", i);
            json_set(assistant, "content", json_str(text));
            json_push(messages, assistant);
        }
        const char *extra = "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"function\":{\"name\":\"bash\",\"arguments\":\"DO_NOT_EXECUTE\"}}]}";
        json_push(messages, json_parse(extra, strlen(extra), NULL));
        extra = "{\"role\":\"tool\",\"content\":\"safe\\u001b[2J output\\u001b[?1049l\"}";
        json_push(messages, json_parse(extra, strlen(extra), NULL));
        extra = "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"imported \\u001b[0mtext\"}]}";
        json_push(messages, json_parse(extra, strlen(extra), NULL));
        char *before = json_dump(messages);
        history_replay(messages, !strcmp(argv[1], "history-chat"));
        char *after = json_dump(messages);
        if (strcmp(before, after)) return 2;
        free(before);
        free(after);
        json_free(messages);
    } else if (argc > 1 && !strcmp(argv[1], "threads")) {
        pthread_t a, b;
        if (pthread_create(&a, NULL, writer, "A") || pthread_create(&b, NULL, writer, "B")) return 1;
        pthread_join(a, NULL);
        pthread_join(b, NULL);
    } else if (argc > 1 && !strcmp(argv[1], "resize-empty")) {
        fprintf(stdout, "\033]resize;8;32\007");
        height = 8; width = 32;
        fs_repaint();
        printf("hello\n");
        fprintf(stdout, "\033]resize;4;20\007");
        height = 4; width = 20;
        fs_repaint();
        printf("world\n");
    } else if (argc > 1 && !strcmp(argv[1], "wrap")) {
        for (int i = 0; i < 40; i++) {
            printf("%024d", i);
            printf("\033[31m");  /* SGR must not cancel pending wrap */
            if (i % 2) fs_repaint();
        }
    } else if (argc > 1 && !strcmp(argv[1], "utf8")) {
        const char *line = "01234567890123456789012界!\n";
        for (int i = 0; i < 20; i++)
            for (const char *p = line; *p; p++) term_write(p, 1);
    } else if (argc > 1 && !strcmp(argv[1], "resize")) {
        for (int i = 0; i < 10; i++) printf("before%02d\n", i);
        /* Test-only OSC markers tell the Python screen model to resize. */
        fprintf(stdout, "\033]resize;4;20\007");
        height = 4; width = 20;
        fs_repaint();
        for (int i = 0; i < 8; i++) printf("middle%02d\n", i);
        fprintf(stdout, "\033]resize;8;32\007");
        height = 8; width = 32;
        fs_repaint();
        for (int i = 0; i < 8; i++) printf("after%02d\n", i);
    } else if (argc > 1 && (!strcmp(argv[1], "prompt") || !strcmp(argv[1], "prompt-bold"))) {
        for (int i = 0; i < 10; i++) printf("line%02d\n", i);
        int interrupted;
        char *answer = prompt_line("> ", 2, 1, &interrupted);
        printf("result:%s\n", answer ? answer : "EOF");
        free(answer);
    } else if (argc > 1 && !strcmp(argv[1], "edit")) {
        for (int i = 0; i < 10; i++) printf("line%02d\n", i);
        term_write("> hello", 7);
        term_write("\r\033[K> world\033[2D!", strlen("\r\033[K> world\033[2D!"));
    } else {
        for (int i = 0; i < 100; i++) printf("line%03d\n", i);
    }
    fs_leave();
    return 0;
}
