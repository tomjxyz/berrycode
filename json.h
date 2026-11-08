/* berrycode - minimal JSON DOM: parse, build, serialize */
#ifndef BC_JSON_H
#define BC_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct json {
    jtype         t;
    double        num;
    char         *str;    /* J_STR (NUL-terminated, UTF-8; may contain embedded NULs via strlen limits) */
    size_t        slen;
    struct json **items;  /* J_ARR / J_OBJ values */
    char        **keys;   /* J_OBJ keys */
    int           len, cap;
} json;

json *json_parse(const char *text, size_t len, const char **err);
void  json_free(json *j);
char *json_dump(const json *j);              /* compact; malloc'd; invalid UTF-8 replaced */
char *json_dump_pretty(const json *j);       /* 2-space indent */
json *json_clone(const json *j);

json *json_null(void);
json *json_bool(int b);
json *json_num(double n);
json *json_strn(const char *s, size_t n);
json *json_str(const char *s);               /* NULL s -> J_NULL */
json *json_arr(void);
json *json_obj(void);

void  json_push(json *arr, json *v);         /* takes ownership */
void  json_set(json *obj, const char *key, json *v);  /* replaces; takes ownership */
json *json_get(const json *obj, const char *key);
json *json_at(const json *arr, int i);
int   json_len(const json *j);

const char *json_gets(const json *obj, const char *key);           /* string or NULL */
double      json_getn(const json *obj, const char *key, double dflt);
int         json_getb(const json *obj, const char *key, int dflt);

/* append a JSON string literal for s (with quotes) to a growing buffer */
void json_quote_into(void *sbuf_ptr, const char *s, size_t n);

#endif
