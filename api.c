/* berrycode - OpenAI-compatible /chat/completions client (libcurl, SSE streaming) */
#include "bc.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include "terminal_io.h"

#define MAX_TC   32
#define RAW_CAP  (4u << 20)

typedef struct { sbuf id, name, args; } tc_acc;

typedef struct {
    sbuf   line;          /* current SSE line */
    sbuf   raw;           /* whole body (capped) for non-SSE / error responses */
    sbuf   content;
    tc_acc tc[MAX_TC];
    int    ntc;
    char  *finish;
    char  *err;
    int    saw_data;
    int    done;
    int    in_reasoning;  /* currently printing reasoning (show_thinking) */
    int    col0;          /* cursor is at the start of a line */
    int    live;          /* inside curl_easy_perform (spinner allowed) */
    int    in_think;      /* inside a <think>...</think> block in the content stream */
    sbuf   pend;          /* possible partial <think> / </think> tag held back */
    int    printed;       /* printed any assistant text */
    mfilter mf;           /* model-output sanitizer (colours kept, controls dropped) */
    mdf    mdx;           /* streaming markdown renderer (display only) */
    long   pt, ct;
} sstate;

void api_global_init(void) { curl_global_init(CURL_GLOBAL_ALL); }

static void end_reasoning(sstate *s)
{
    if (!s->in_reasoning) return;
    printf("%s\n", C_RESET);
    s->in_reasoning = 0;
    s->col0 = 1;
}

/* (re)start the berry animation on a fresh line */
static void busy(sstate *s)
{
    if (!s->live || spinner_active()) return;
    end_reasoning(s);
    if (!s->col0) { printf("\n"); s->col0 = 1; }
    spinner_start();
}

/* reasoning text: printed dim with show_thinking, otherwise hidden behind the spinner */
static void out_reasoning(sstate *s, const char *t, size_t n)
{
    if (!n) return;
    if (!g_cfg.show_thinking) { busy(s); return; }
    spinner_stop();
    if (!s->in_reasoning) {
        if (!s->col0) printf("\n");
        printf("%s", C_DIM);
        s->in_reasoning = 1;
    }
    /* colours the model emits survive (Term49CX renders them); cursor
     * moves, clears, OSC 52 clipboard and friends are dropped. */
    sbuf plain = {0}, disp = {0};
    mf_add(&s->mf, t, n, &plain, &disp);
    sb_free(&plain);                       /* reasoning is display-only */
    if (disp.n) {
        fwrite(disp.p, 1, disp.n, stdout);
        s->col0 = disp.p[disp.n - 1] == '\n';
    }
    sb_free(&disp);
    fflush(stdout);
}

/* visible answer text; also what goes into the conversation history */
static void out_content(sstate *s, const char *t, size_t n)
{
    if (!s->printed) {                      /* drop blank lines left over after a think block */
        while (n && (*t == '\n' || *t == '\r' || *t == ' ' || *t == '\t')) { t++; n--; }
        if (!n) return;
    }
    spinner_stop();
    end_reasoning(s);
    sbuf plain = {0}, disp = {0};
    mf_add(&s->mf, t, n, &plain, &disp);
    if (disp.n) {
        sbuf rich = {0};
        md_add(&s->mdx, disp.p, disp.n, &rich);   /* markdown -> ANSI */
        if (rich.n) {
            fwrite(rich.p, 1, rich.n, stdout);
            s->col0 = disp.p[disp.n - 1] == '\n';
        }
        sb_free(&rich);
    }
    fflush(stdout);
    sb_add(&s->content, plain.p, plain.n);  /* history keeps the plain text */
    sb_free(&plain);
    sb_free(&disp);
    s->printed = 1;
}

/* content may carry inline reasoning as <think>...</think> (GLM, DeepSeek, Qwen...):
 * route it to out_reasoning; tags can be split across stream chunks */
static void take_content(sstate *s, const char *t, size_t n)
{
    size_t run = 0;                           /* start of the pending plain run */
    for (size_t i = 0; i < n; i++) {
        const char *tag = s->in_think ? "</think>" : "<think>";
        size_t tl = strlen(tag);
        if (s->pend.n || t[i] == '<') {
            if (!s->pend.n) {                 /* flush text before the '<' */
                if (s->in_think) out_reasoning(s, t + run, i - run); else out_content(s, t + run, i - run);
            }
            sb_addc(&s->pend, t[i]);
            run = i + 1;
            if (s->pend.n <= tl && !memcmp(s->pend.p, tag, s->pend.n)) {
                if (s->pend.n == tl) { s->in_think = !s->in_think; s->pend.n = 0; }
                continue;                     /* still a possible tag */
            }
            /* not a tag after all: emit what we held back (a trailing '<' may start a new tag) */
            size_t k = s->pend.n;
            int keep_lt = k > 1 && s->pend.p[k - 1] == '<';
            if (keep_lt) k--;
            s->pend.n = 0;
            if (s->in_think) out_reasoning(s, s->pend.p, k); else out_content(s, s->pend.p, k);
            if (keep_lt) sb_addc(&s->pend, '<');
        }
    }
    if (run < n) { if (s->in_think) out_reasoning(s, t + run, n - run); else out_content(s, t + run, n - run); }
}

static void flush_pending(sstate *s)
{
    if (s->pend.n && !s->in_think) out_content(s, s->pend.p, s->pend.n);
    s->pend.n = 0;
    mf_flush(&s->mf);          /* drop any partial escape at end of stream */
    {   /* close markdown spans / render leftovers literally */
        sbuf tail = {0};
        md_end(&s->mdx, &tail);
        if (tail.n) fwrite(tail.p, 1, tail.n, stdout);
        sb_free(&tail);
        fflush(stdout);
    }
    end_reasoning(s);
}

static void take_usage(sstate *s, const json *ev)
{
    json *u = json_get(ev, "usage");
    if (u && u->t == J_OBJ) {
        s->pt = (long)json_getn(u, "prompt_tokens", (double)s->pt);
        s->ct = (long)json_getn(u, "completion_tokens", (double)s->ct);
    }
}

static const char *error_text(const json *j)
{
    json *e = json_get(j, "error");
    if (e && e->t == J_STR) return e->str;
    if (e && e->t == J_OBJ) {
        const char *m = json_gets(e, "message");
        if (m) return m;
    }
    const char *m = json_gets(j, "message");
    if (m && !json_get(j, "choices")) return m;
    return NULL;
}

/* merge a tool_calls array (delta or full) into the accumulators */
static void take_tool_calls(sstate *s, const json *tcs)
{
    busy(s);              /* big tool arguments (whole files) can take a while to arrive */
    for (int i = 0; i < json_len(tcs); i++) {
        json *tc = json_at(tcs, i);
        int idx = (int)json_getn(tc, "index", -1);
        if (idx < 0) idx = s->ntc > 0 && !json_gets(tc, "id") ? s->ntc - 1 : s->ntc;
        if (idx >= MAX_TC) continue;
        if (idx >= s->ntc) s->ntc = idx + 1;
        const char *id = json_gets(tc, "id");
        if (id && *id && s->tc[idx].id.n == 0) sb_adds(&s->tc[idx].id, id);
        json *fn = json_get(tc, "function");
        const char *nm = json_gets(fn, "name");
        if (nm) sb_adds(&s->tc[idx].name, nm);
        json *a = json_get(fn, "arguments");
        if (a && a->t == J_STR) sb_add(&s->tc[idx].args, a->str, a->slen);
        else if (a && a->t == J_OBJ) { char *d = json_dump(a); sb_adds(&s->tc[idx].args, d); free(d); }
    }
}

static void handle_event(sstate *s, const json *ev)
{
    const char *e = error_text(ev);
    if (e) { free(s->err); s->err = xstrdup(e); return; }
    take_usage(s, ev);
    json *ch = json_at(json_get(ev, "choices"), 0);
    if (!ch) return;
    json *d = json_get(ch, "delta");
    if (!d) d = json_get(ch, "message");     /* some servers send full messages */
    if (d) {
        const char *r = json_gets(d, "reasoning_content");
        if (!r) r = json_gets(d, "reasoning");
        if (r) out_reasoning(s, r, strlen(r));
        json *c = json_get(d, "content");
        if (c && c->t == J_STR) take_content(s, c->str, c->slen);
        json *tcs = json_get(d, "tool_calls");
        if (tcs) take_tool_calls(s, tcs);
    }
    const char *fr = json_gets(ch, "finish_reason");
    if (fr) { free(s->finish); s->finish = xstrdup(fr); }
}

static void handle_line(sstate *s, char *line)
{
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[--n] = 0;
    if (strncmp(line, "data:", 5) != 0) return;
    char *p = line + 5;
    while (*p == ' ') p++;
    s->saw_data = 1;
    if (strcmp(p, "[DONE]") == 0) { s->done = 1; return; }
    json *ev = json_parse(p, strlen(p), NULL);
    if (ev) { handle_event(s, ev); json_free(ev); }
}

static size_t on_data(char *ptr, size_t sz, size_t nm, void *ud)
{
    sstate *s = ud;
    size_t n = sz * nm;
    if (s->raw.n < RAW_CAP) sb_add(&s->raw, ptr, n);
    for (size_t i = 0; i < n; i++) {
        if (ptr[i] == '\n') {
            if (s->line.p) handle_line(s, s->line.p);
            s->line.n = 0;
            if (s->line.p) s->line.p[0] = 0;
        } else {
            sb_addc(&s->line, ptr[i]);
        }
    }
    return n;
}

static size_t on_plain(char *ptr, size_t sz, size_t nm, void *ud)
{
    sbuf *b = ud;
    size_t n = sz * nm;
    if (b->n < RAW_CAP) sb_add(b, ptr, n);
    return n;
}

static int on_progress(void *ud, double dt, double dn, double ut, double un)
{
    (void)ud; (void)dt; (void)dn; (void)ut; (void)un;
    return g_interrupted ? 1 : 0;
}

static struct curl_slist *common_setup(CURL *c, const char *url)
{
    struct curl_slist *h = NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "berrycode/" BC_VERSION);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)g_cfg.timeout);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_ENCODING, "");    /* accept gzip if the server offers it */
    if (g_cfg.insecure) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        if (g_cfg.ca_bundle) curl_easy_setopt(c, CURLOPT_CAINFO, g_cfg.ca_bundle);
    }
    if (g_cfg.api_key && *g_cfg.api_key) {
        sbuf a = {0};
        sb_printf(&a, "Authorization: Bearer %s", g_cfg.api_key);
        h = curl_slist_append(h, a.p);
        sb_free(&a);
    }
    if (g_cfg.opencode_session && g_session_id && *g_session_id) {
        sbuf s = {0};
        sb_printf(&s, "x-opencode-session: %s", g_session_id);
        h = curl_slist_append(h, s.p);
        sb_free(&s);
    }
    if (g_cfg.extra_headers) {
        char *copy = xstrdup(g_cfg.extra_headers), *save = NULL;
        for (char *t = strtok_r(copy, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
            while (*t == ' ') t++;
            if (*t) h = curl_slist_append(h, t);
        }
        free(copy);
    }
    return h;
}

static char *join_url(const char *path)
{
    sbuf u = {0};
    sb_adds(&u, g_cfg.base_url);
    while (u.n && u.p[u.n - 1] == '/') u.p[--u.n] = 0;
    sb_adds(&u, path);
    return sb_take(&u);
}

static char *body_error(const char *raw, size_t n, long status)
{
    sbuf e = {0};
    json *j = json_parse(raw, n, NULL);
    const char *m = j ? error_text(j) : NULL;
    if (m) sb_printf(&e, "HTTP %ld: %s", status, m);
    else sb_printf(&e, "HTTP %ld: %.300s", status, n ? raw : "(empty response)");
    json_free(j);
    return sb_take(&e);
}

int api_chat(json *messages, const json *tools, chat_result *out)
{
    memset(out, 0, sizeof *out);
    json *body = json_obj();
    json_set(body, "model", json_str(g_cfg.model));
    json_set(body, "messages", json_clone(messages));
    if (tools && json_len(tools)) json_set(body, "tools", json_clone(tools));
    if (g_cfg.stream) {
        json_set(body, "stream", json_bool(1));
        json *options = json_obj();
        json_set(options, "include_usage", json_bool(1));
        json_set(body, "stream_options", options);
    }
    if (g_cfg.max_tokens > 0) json_set(body, "max_tokens", json_num(g_cfg.max_tokens));
    if (g_cfg.temperature >= 0) json_set(body, "temperature", json_num(g_cfg.temperature));
    char *payload = json_dump(body);
    json_free(body);

    char *url = join_url("/chat/completions");
    CURL *c = curl_easy_init();
    sstate s;
    memset(&s, 0, sizeof s);
    struct curl_slist *h = common_setup(c, url);
    h = curl_slist_append(h, "Content-Type: application/json");
    if (g_cfg.stream) h = curl_slist_append(h, "Accept: text/event-stream");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);

    s.col0 = 1;
    s.live = 1;
    spinner_start();
    CURLcode rc = curl_easy_perform(c);
    s.live = 0;
    spinner_stop();
    if (s.line.n) handle_line(&s, s.line.p);       /* last line without '\n' */
    flush_pending(&s);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->http_status);

    if (rc == CURLE_ABORTED_BY_CALLBACK || g_interrupted) {
        out->cancelled = 1;
        out->error = xstrdup("cancelled");
    } else if (rc != CURLE_OK) {
        sbuf e = {0};
        sb_printf(&e, "network error: %s", curl_easy_strerror(rc));
        if (rc == CURLE_SSL_CACERT || rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PEER_FAILED_VERIFICATION)
            sb_adds(&e, " (TLS problem: check ca_bundle, or try insecure=true / an http:// endpoint)");
        out->error = sb_take(&e);
    } else if (out->http_status >= 400) {
        out->error = body_error(sb_str(&s.raw), s.raw.n, out->http_status);
    } else if (!s.saw_data) {
        /* plain JSON response (stream=false, or a server that ignores stream) */
        const char *err = NULL;
        json *j = json_parse(sb_str(&s.raw), s.raw.n, &err);
        if (!j) {
            sbuf e = {0};
            sb_printf(&e, "bad response (%s): %.200s", err, sb_str(&s.raw));
            out->error = sb_take(&e);
        } else {
            handle_event(&s, j);
            flush_pending(&s);
            json_free(j);
        }
    }
    if (!out->error && s.err) out->error = xstrdup(s.err);
    spinner_stop();
    end_reasoning(&s);
    if (!s.col0) printf("\n");

    out->content = sb_take(&s.content);
    out->finish_reason = s.finish;
    out->prompt_tokens = s.pt;
    out->completion_tokens = s.ct;
    if (s.ntc > 0) {
        out->tool_calls = json_arr();
        for (int i = 0; i < s.ntc; i++) {
            if (!s.tc[i].name.n) continue;
            json *tc = json_obj();
            char idbuf[32];
            snprintf(idbuf, sizeof idbuf, "call_%d", i);
            json_set(tc, "id", json_str(s.tc[i].id.n ? s.tc[i].id.p : idbuf));
            json_set(tc, "type", json_str("function"));
            json *fn = json_obj();
            json_set(fn, "name", json_str(sb_str(&s.tc[i].name)));
            json_set(fn, "arguments", json_str(s.tc[i].args.n ? s.tc[i].args.p : "{}"));
            json_set(tc, "function", fn);
            json_push(out->tool_calls, tc);
        }
        if (!json_len(out->tool_calls)) { json_free(out->tool_calls); out->tool_calls = NULL; }
    }
    for (int i = 0; i < MAX_TC; i++) { sb_free(&s.tc[i].id); sb_free(&s.tc[i].name); sb_free(&s.tc[i].args); }
    free(s.err);
    sb_free(&s.line);
    sb_free(&s.pend);
    sb_free(&s.raw);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    free(url);
    free(payload);
    return out->error ? -1 : 0;
}

char *api_get(const char *path, long *status, char **err)
{
    char *url = join_url(path);
    CURL *c = curl_easy_init();
    sbuf b = {0};
    struct curl_slist *h = common_setup(c, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_plain);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    CURLcode rc = curl_easy_perform(c);
    *status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    *err = NULL;
    if (rc != CURLE_OK) {
        sbuf e = {0};
        sb_printf(&e, "%s", rc == CURLE_ABORTED_BY_CALLBACK ? "cancelled" : curl_easy_strerror(rc));
        *err = sb_take(&e);
    } else if (*status >= 400) {
        *err = body_error(sb_str(&b), b.n, *status);
    }
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    free(url);
    if (*err) { sb_free(&b); return NULL; }
    return sb_take(&b);
}

void chat_result_free(chat_result *r)
{
    free(r->content);
    json_free(r->tool_calls);
    free(r->finish_reason);
    free(r->error);
    memset(r, 0, sizeof *r);
}
