#include "json.h"
#include "util.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- construction ---------- */

static json *jnew(jtype t)
{
    json *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->t = t;
    return j;
}

json *json_null(void)    { return jnew(J_NULL); }
json *json_bool(int b)   { return jnew(b ? J_TRUE : J_FALSE); }
json *json_arr(void)     { return jnew(J_ARR); }
json *json_obj(void)     { return jnew(J_OBJ); }
json *json_num(double n) { json *j = jnew(J_NUM); j->num = n; return j; }

json *json_strn(const char *s, size_t n)
{
    json *j = jnew(J_STR);
    j->str = xstrndup(s, n);
    j->slen = n;
    return j;
}

json *json_str(const char *s) { return s ? json_strn(s, strlen(s)) : json_null(); }

static void jgrow(json *j)
{
    if (j->len < j->cap) return;
    j->cap = j->cap ? j->cap * 2 : 8;
    j->items = xrealloc(j->items, sizeof(json *) * (size_t)j->cap);
    if (j->t == J_OBJ) j->keys = xrealloc(j->keys, sizeof(char *) * (size_t)j->cap);
}

void json_push(json *arr, json *v)
{
    if (!arr || arr->t != J_ARR) { json_free(v); return; }
    jgrow(arr);
    arr->items[arr->len++] = v;
}

void json_set(json *obj, const char *key, json *v)
{
    if (!obj || obj->t != J_OBJ) { json_free(v); return; }
    for (int i = 0; i < obj->len; i++)
        if (strcmp(obj->keys[i], key) == 0) { json_free(obj->items[i]); obj->items[i] = v; return; }
    jgrow(obj);
    obj->keys[obj->len] = xstrdup(key);
    obj->items[obj->len++] = v;
}

json *json_get(const json *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    for (int i = 0; i < obj->len; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

json *json_at(const json *arr, int i)
{
    if (!arr || (arr->t != J_ARR && arr->t != J_OBJ) || i < 0 || i >= arr->len) return NULL;
    return arr->items[i];
}

int json_len(const json *j) { return (j && (j->t == J_ARR || j->t == J_OBJ)) ? j->len : 0; }

const char *json_gets(const json *obj, const char *key)
{
    json *v = json_get(obj, key);
    return (v && v->t == J_STR) ? v->str : NULL;
}

double json_getn(const json *obj, const char *key, double dflt)
{
    json *v = json_get(obj, key);
    return (v && v->t == J_NUM) ? v->num : dflt;
}

int json_getb(const json *obj, const char *key, int dflt)
{
    json *v = json_get(obj, key);
    if (!v) return dflt;
    if (v->t == J_TRUE) return 1;
    if (v->t == J_FALSE) return 0;
    if (v->t == J_NUM) return v->num != 0;
    if (v->t == J_STR) return strcmp(v->str, "true") == 0 || strcmp(v->str, "1") == 0;
    return dflt;
}

void json_free(json *j)
{
    if (!j) return;
    for (int i = 0; i < j->len; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

json *json_clone(const json *j)
{
    if (!j) return NULL;
    json *c = jnew(j->t);
    c->num = j->num;
    if (j->t == J_STR) { c->str = xstrndup(j->str, j->slen); c->slen = j->slen; }
    for (int i = 0; i < j->len; i++) {
        if (j->t == J_OBJ) json_set(c, j->keys[i], json_clone(j->items[i]));
        else json_push(c, json_clone(j->items[i]));
    }
    return c;
}

/* ---------- parser ---------- */

typedef struct { const char *p, *end; const char *err; int depth; } jp;

static void ws(jp *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static void put_utf8(sbuf *b, uint32_t cp)
{
    char o[4];
    if (cp < 0x80) { o[0] = (char)cp; sb_add(b, o, 1); }
    else if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); sb_add(b, o, 2); }
    else if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); sb_add(b, o, 3);
    } else {
        o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); sb_add(b, o, 4);
    }
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static json *parse_value(jp *s);

static char *parse_string_raw(jp *s, size_t *outlen)
{
    if (s->p >= s->end || *s->p != '"') { s->err = "expected string"; return NULL; }
    s->p++;
    sbuf b = {0};
    sb_add(&b, "", 0);
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c != '\\') { sb_addc(&b, c); continue; }
        if (s->p >= s->end) break;
        char e = *s->p++;
        switch (e) {
        case '"': sb_addc(&b, '"'); break;
        case '\\': sb_addc(&b, '\\'); break;
        case '/': sb_addc(&b, '/'); break;
        case 'b': sb_addc(&b, '\b'); break;
        case 'f': sb_addc(&b, '\f'); break;
        case 'n': sb_addc(&b, '\n'); break;
        case 'r': sb_addc(&b, '\r'); break;
        case 't': sb_addc(&b, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (s->end - s->p < 4 || hex4(s->p, &cp)) { s->err = "bad \\u escape"; sb_free(&b); return NULL; }
            s->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                uint32_t lo;
                if (!hex4(s->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s->p += 6;
                }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            put_utf8(&b, cp);
            break;
        }
        default: s->err = "bad escape"; sb_free(&b); return NULL;
        }
    }
    if (s->p >= s->end) { s->err = "unterminated string"; sb_free(&b); return NULL; }
    s->p++;
    *outlen = b.n;
    return sb_take(&b);
}

static json *parse_value(jp *s)
{
    ws(s);
    if (s->p >= s->end) { s->err = "unexpected end"; return NULL; }
    if (++s->depth > 200) { s->err = "nesting too deep"; return NULL; }
    json *r = NULL;
    char c = *s->p;
    if (c == '{') {
        s->p++;
        r = json_obj();
        ws(s);
        if (s->p < s->end && *s->p == '}') { s->p++; goto done; }
        for (;;) {
            ws(s);
            size_t kl;
            char *k = parse_string_raw(s, &kl);
            if (!k) goto fail;
            ws(s);
            if (s->p >= s->end || *s->p != ':') { free(k); s->err = "expected ':'"; goto fail; }
            s->p++;
            json *v = parse_value(s);
            if (!v) { free(k); goto fail; }
            json_set(r, k, v);
            free(k);
            ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; continue; }
            if (s->p < s->end && *s->p == '}') { s->p++; break; }
            s->err = "expected ',' or '}'";
            goto fail;
        }
    } else if (c == '[') {
        s->p++;
        r = json_arr();
        ws(s);
        if (s->p < s->end && *s->p == ']') { s->p++; goto done; }
        for (;;) {
            json *v = parse_value(s);
            if (!v) goto fail;
            json_push(r, v);
            ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; continue; }
            if (s->p < s->end && *s->p == ']') { s->p++; break; }
            s->err = "expected ',' or ']'";
            goto fail;
        }
    } else if (c == '"') {
        size_t n;
        char *str = parse_string_raw(s, &n);
        if (!str) goto fail;
        r = jnew(J_STR);
        r->str = str;
        r->slen = n;
    } else if (c == 't' && s->end - s->p >= 4 && !memcmp(s->p, "true", 4)) { s->p += 4; r = json_bool(1); }
    else if (c == 'f' && s->end - s->p >= 5 && !memcmp(s->p, "false", 5)) { s->p += 5; r = json_bool(0); }
    else if (c == 'n' && s->end - s->p >= 4 && !memcmp(s->p, "null", 4)) { s->p += 4; r = json_null(); }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (s->p < s->end && n < sizeof tmp - 1 &&
               (strchr("+-.eE", *s->p) || (*s->p >= '0' && *s->p <= '9')))
            tmp[n++] = *s->p++;
        tmp[n] = 0;
        char *e;
        double d = strtod(tmp, &e);
        if (e == tmp) { s->err = "bad number"; goto fail; }
        r = json_num(d);
    } else {
        s->err = "unexpected character";
        goto fail;
    }
done:
    s->depth--;
    return r;
fail:
    json_free(r);
    return NULL;
}

json *json_parse(const char *text, size_t len, const char **err)
{
    jp s = { text, text + len, NULL, 0 };
    json *r = parse_value(&s);
    if (r) {
        ws(&s);
        if (s.p != s.end) { json_free(r); r = NULL; s.err = "trailing data"; }
    }
    if (err) *err = r ? NULL : (s.err ? s.err : "parse error");
    return r;
}

/* ---------- serializer ---------- */

/* length of a valid UTF-8 sequence at p (1..4), or 0 if invalid */
static int utf8_seq(const unsigned char *p, const unsigned char *end)
{
    unsigned c = p[0];
    int n;
    uint32_t cp;
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (end - p < n) return 0;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
        return 0;
    return n;
}

void json_quote_into(void *bp, const char *s, size_t n)
{
    sbuf *b = bp;
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    sb_addc(b, '"');
    while (p < end) {
        unsigned c = *p;
        if (c == '"') { sb_add(b, "\\\"", 2); p++; }
        else if (c == '\\') { sb_add(b, "\\\\", 2); p++; }
        else if (c == '\n') { sb_add(b, "\\n", 2); p++; }
        else if (c == '\r') { sb_add(b, "\\r", 2); p++; }
        else if (c == '\t') { sb_add(b, "\\t", 2); p++; }
        else if (c < 0x20 || c == 0x7F) { sb_printf(b, "\\u%04x", c); p++; }
        else if (c < 0x80) { sb_addc(b, (char)c); p++; }
        else {
            int k = utf8_seq(p, end);
            if (k == 0) { sb_add(b, "\xEF\xBF\xBD", 3); p++; }
            else { sb_add(b, (const char *)p, (size_t)k); p += k; }
        }
    }
    sb_addc(b, '"');
}

static void indent(sbuf *b, int pretty, int lvl)
{
    if (!pretty) return;
    sb_addc(b, '\n');
    for (int i = 0; i < lvl; i++) sb_add(b, "  ", 2);
}

static void dump(sbuf *b, const json *j, int pretty, int lvl)
{
    switch (j ? j->t : J_NULL) {
    case J_NULL: sb_adds(b, "null"); break;
    case J_TRUE: sb_adds(b, "true"); break;
    case J_FALSE: sb_adds(b, "false"); break;
    case J_NUM:
        if (!isfinite(j->num)) sb_adds(b, "null");
        else if (j->num == (double)(long long)j->num && fabs(j->num) < 1e15) sb_printf(b, "%lld", (long long)j->num);
        else sb_printf(b, "%.17g", j->num);
        break;
    case J_STR: json_quote_into(b, j->str, j->slen); break;
    case J_ARR:
    case J_OBJ: {
        int obj = j->t == J_OBJ;
        sb_addc(b, obj ? '{' : '[');
        for (int i = 0; i < j->len; i++) {
            if (i) sb_addc(b, ',');
            indent(b, pretty, lvl + 1);
            if (obj) {
                json_quote_into(b, j->keys[i], strlen(j->keys[i]));
                sb_adds(b, pretty ? ": " : ":");
            }
            dump(b, j->items[i], pretty, lvl + 1);
        }
        if (j->len) indent(b, pretty, lvl);
        sb_addc(b, obj ? '}' : ']');
        break;
    }
    }
}

char *json_dump(const json *j)
{
    sbuf b = {0};
    dump(&b, j, 0, 0);
    return sb_take(&b);
}

char *json_dump_pretty(const json *j)
{
    sbuf b = {0};
    dump(&b, j, 1, 0);
    sb_addc(&b, '\n');
    return sb_take(&b);
}
