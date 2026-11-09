/* Offline CLI integration tests: no curl headers/library or network access. */
#include "bc.h"
#include <stdlib.h>
#include <string.h>

void api_global_init(void) { }
int api_chat(json *messages, const json *tools, chat_result *result)
{
    (void)messages;
    (void)tools;
    memset(result, 0, sizeof *result);
    result->content = xstrdup("offline test response");
    result->prompt_tokens = 1200;
    result->completion_tokens = 250;
    term_write("offline test response\n", strlen("offline test response\n"));
    return 0;
}
void chat_result_free(chat_result *result)
{
    free(result->content);
    free(result->finish_reason);
    free(result->error);
    json_free(result->tool_calls);
}
char *api_get(const char *path, long *status, char **error)
{
    (void)path;
    *status = 0;
    *error = xstrdup("network disabled in tests");
    return NULL;
}
json *tools_schema(void) { return json_arr(); }
void tools_reset_session(void) { }
char *tool_run(const char *name, const char *args)
{
    (void)name;
    (void)args;
    abort(); /* Replaying a saved tool call must NEVER execute it. */
}
