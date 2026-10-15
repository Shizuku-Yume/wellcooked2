/*
 * json.c -- recursive-descent JSON parser and a buffer-backed writer.
 *
 * Hand-written on purpose (see decisions.md 2026-10-15): the course machines
 * cannot be assumed to have network access for a package, and the game plugin
 * only ever emits this subset.
 */
#include <kitchenos/json.h>

#include <errno.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* growable byte buffer: only used while a string or the input is built */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} jbuf;

static void jbuf_init(jbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void jbuf_free(jbuf *b)
{
    free(b->data);
    jbuf_init(b);
}

static int jbuf_reserve(jbuf *b, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    if (extra > (size_t)-1 - b->len)
        return JSON_ERR_NOMEM;
    need = b->len + extra;
    if (need <= b->cap)
        return JSON_OK;

    cap = b->cap ? b->cap : 32;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2)
            return JSON_ERR_NOMEM;
        cap *= 2;
    }
    grown = realloc(b->data, cap);
    if (grown == NULL)
        return JSON_ERR_NOMEM;
    b->data = grown;
    b->cap = cap;
    return JSON_OK;
}

static int jbuf_put(jbuf *b, const char *bytes, size_t len)
{
    int rc = jbuf_reserve(b, len);
    if (rc != JSON_OK)
        return rc;
    memcpy(b->data + b->len, bytes, len);
    b->len += len;
    return JSON_OK;
}

static int jbuf_putc(jbuf *b, char c)
{
    return jbuf_put(b, &c, 1);
}

/* ------------------------------------------------------------------ */
/* document nodes                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;
    size_t key_len;
    json_value *value;
} json_member;

struct json_value {
    json_type type;
    union {
        int boolean;
        double number;
        struct {
            char *ptr;
            size_t len;
        } string;
        struct {
            json_value **items;
            size_t count;
            size_t cap;
        } array;
        struct {
            json_member *members;
            size_t count;
            size_t cap;
        } object;
    } u;
};

static json_value *jv_new(json_type type)
{
    json_value *v = calloc(1, sizeof *v);
    if (v != NULL)
        v->type = type;
    return v;
}

void json_free(json_value *value)
{
    size_t i;

    if (value == NULL)
        return;
    switch (value->type) {
    case JSON_STRING:
        free(value->u.string.ptr);
        break;
    case JSON_ARRAY:
        for (i = 0; i < value->u.array.count; i++)
            json_free(value->u.array.items[i]);
        free(value->u.array.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < value->u.object.count; i++) {
            free(value->u.object.members[i].key);
            json_free(value->u.object.members[i].value);
        }
        free(value->u.object.members);
        break;
    default:
        break;
    }
    free(value);
}

static int jv_array_push(json_value *v, json_value *item)
{
    size_t cap;
    json_value **grown;

    if (v->u.array.count == v->u.array.cap) {
        cap = v->u.array.cap ? v->u.array.cap * 2 : 4;
        if (cap < v->u.array.cap)
            return JSON_ERR_NOMEM;
        grown = realloc(v->u.array.items, cap * sizeof *grown);
        if (grown == NULL)
            return JSON_ERR_NOMEM;
        v->u.array.items = grown;
        v->u.array.cap = cap;
    }
    v->u.array.items[v->u.array.count++] = item;
    return JSON_OK;
}

static int jv_object_push(json_value *v, char *key, size_t key_len, json_value *item)
{
    size_t cap;
    json_member *grown;

    if (v->u.object.count == v->u.object.cap) {
        cap = v->u.object.cap ? v->u.object.cap * 2 : 4;
        if (cap < v->u.object.cap)
            return JSON_ERR_NOMEM;
        grown = realloc(v->u.object.members, cap * sizeof *grown);
        if (grown == NULL)
            return JSON_ERR_NOMEM;
        v->u.object.members = grown;
        v->u.object.cap = cap;
    }
    v->u.object.members[v->u.object.count].key = key;
    v->u.object.members[v->u.object.count].key_len = key_len;
    v->u.object.members[v->u.object.count].value = item;
    v->u.object.count++;
    return JSON_OK;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    int depth;
    json_error *err;
} jparse;

void json_error_init(json_error *err)
{
    if (err == NULL)
        return;
    err->code = JSON_OK;
    err->offset = 0;
    err->line = 1;
    err->column = 1;
    err->sys_errno = 0;
    err->detail[0] = '\0';
}

const char *json_strerror(int code)
{
    switch (code) {
    case JSON_OK: return "ok";
    case JSON_ERR_SYNTAX: return "malformed JSON";
    case JSON_ERR_TRUNCATED: return "input ended inside a value";
    case JSON_ERR_TRAILING: return "trailing bytes after the JSON value";
    case JSON_ERR_DEPTH: return "JSON nesting too deep";
    case JSON_ERR_UTF8: return "invalid \\u escape";
    case JSON_ERR_NOMEM: return "out of memory";
    case JSON_ERR_NOSPACE: return "buffer too small";
    case JSON_ERR_RANGE: return "number out of range";
    case JSON_ERR_TYPE: return "unexpected JSON type";
    case JSON_ERR_MISSING: return "member not present";
    default: return "unknown JSON error";
    }
}

/* Records the first failure only: later ones describe the fallout, not the bug. */
static int jp_fail(jparse *p, int code, size_t at, const char *fmt, ...)
{
    json_error *e = p->err;
    va_list ap;
    size_t i;
    size_t stop = at < p->len ? at : p->len;
    int line = 1;
    int column = 1;

    if (e != NULL && e->code == JSON_OK) {
        e->code = code;
        e->offset = at;
        e->sys_errno = (code == JSON_ERR_NOMEM) ? errno : 0;
        va_start(ap, fmt);
        vsnprintf(e->detail, sizeof e->detail, fmt, ap);
        va_end(ap);
        for (i = 0; i < stop; i++) {
            if (p->text[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        e->line = line;
        e->column = column;
    }
    return code;
}

static void jp_ws(jparse *p)
{
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int jp_hex4(jparse *p, unsigned *out)
{
    unsigned value = 0;
    int i;

    if (p->len - p->pos < 4)
        return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "incomplete \\u escape");
    for (i = 0; i < 4; i++) {
        char c = p->text[p->pos + (size_t)i];
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= (unsigned)(c - 'A' + 10);
        else
            return jp_fail(p, JSON_ERR_UTF8, p->pos + (size_t)i,
                           "'%c' is not a hex digit in a \\u escape", c);
    }
    p->pos += 4;
    *out = value;
    return JSON_OK;
}

static int jp_utf8_encode(jbuf *b, unsigned codepoint)
{
    char bytes[4];
    size_t n;

    if (codepoint < 0x80u) {
        bytes[0] = (char)codepoint;
        n = 1;
    } else if (codepoint < 0x800u) {
        bytes[0] = (char)(0xC0u | (codepoint >> 6));
        bytes[1] = (char)(0x80u | (codepoint & 0x3Fu));
        n = 2;
    } else if (codepoint < 0x10000u) {
        bytes[0] = (char)(0xE0u | (codepoint >> 12));
        bytes[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[2] = (char)(0x80u | (codepoint & 0x3Fu));
        n = 3;
    } else {
        bytes[0] = (char)(0xF0u | (codepoint >> 18));
        bytes[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        bytes[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[3] = (char)(0x80u | (codepoint & 0x3Fu));
        n = 4;
    }
    return jbuf_put(b, bytes, n);
}

/* Reads the opening quote and everything up to the closing one into a fresh
 * NUL-terminated allocation. Raw UTF-8 bytes are passed through unchanged: the
 * wire is UTF-8 by contract, and re-validating every byte would cost more than
 * it catches. Unescaped control characters are rejected, as JSON requires. */
static int jp_string(jparse *p, char **out, size_t *out_len)
{
    jbuf b;
    int rc = JSON_OK;

    jbuf_init(&b);
    if (p->pos >= p->len)
        return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "string starts past the end");
    p->pos++; /* opening quote */

    for (;;) {
        char c;
        if (p->pos >= p->len) {
            rc = jp_fail(p, JSON_ERR_TRUNCATED, p->len, "unterminated string");
            break;
        }
        c = p->text[p->pos++];
        if (c == '"')
            break;
        if (c == '\\') {
            char esc;
            if (p->pos >= p->len) {
                rc = jp_fail(p, JSON_ERR_TRUNCATED, p->len, "unterminated escape");
                break;
            }
            esc = p->text[p->pos++];
            switch (esc) {
            case '"': rc = jbuf_putc(&b, '"'); break;
            case '\\': rc = jbuf_putc(&b, '\\'); break;
            case '/': rc = jbuf_putc(&b, '/'); break;
            case 'b': rc = jbuf_putc(&b, '\b'); break;
            case 'f': rc = jbuf_putc(&b, '\f'); break;
            case 'n': rc = jbuf_putc(&b, '\n'); break;
            case 'r': rc = jbuf_putc(&b, '\r'); break;
            case 't': rc = jbuf_putc(&b, '\t'); break;
            case 'u': {
                unsigned high;
                rc = jp_hex4(p, &high);
                if (rc != JSON_OK)
                    break;
                if (high >= 0xD800u && high <= 0xDBFFu) {
                    unsigned low;
                    if (p->len - p->pos < 6 || p->text[p->pos] != '\\' ||
                        p->text[p->pos + 1] != 'u') {
                        rc = jp_fail(p, JSON_ERR_UTF8, p->pos,
                                     "high surrogate U+%04X without a low surrogate",
                                     high);
                        break;
                    }
                    p->pos += 2;
                    rc = jp_hex4(p, &low);
                    if (rc != JSON_OK)
                        break;
                    if (low < 0xDC00u || low > 0xDFFFu) {
                        rc = jp_fail(p, JSON_ERR_UTF8, p->pos,
                                     "U+%04X is not a low surrogate", low);
                        break;
                    }
                    rc = jp_utf8_encode(&b, 0x10000u + ((high - 0xD800u) << 10) +
                                                (low - 0xDC00u));
                } else if (high >= 0xDC00u && high <= 0xDFFFu) {
                    rc = jp_fail(p, JSON_ERR_UTF8, p->pos,
                                 "unpaired low surrogate U+%04X", high);
                } else {
                    rc = jp_utf8_encode(&b, high);
                }
                break;
            }
            default:
                rc = jp_fail(p, JSON_ERR_SYNTAX, p->pos - 1, "invalid escape \\%c", esc);
                break;
            }
        } else if ((unsigned char)c < 0x20u) {
            rc = jp_fail(p, JSON_ERR_SYNTAX, p->pos - 1,
                         "unescaped control character 0x%02X", (unsigned)(unsigned char)c);
        } else {
            rc = jbuf_putc(&b, c);
        }
        if (rc != JSON_OK)
            break;
    }

    if (rc == JSON_OK) {
        char *copy = malloc(b.len + 1);
        if (copy == NULL) {
            rc = jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for a %lu byte string",
                         (unsigned long)b.len);
        } else {
            if (b.len > 0)
                memcpy(copy, b.data, b.len);
            copy[b.len] = '\0';
            *out = copy;
            if (out_len != NULL)
                *out_len = b.len;
        }
    }
    jbuf_free(&b);
    return rc;
}

static int jp_number(jparse *p, double *out)
{
    size_t start = p->pos;
    size_t digits = 0;
    char literal[64];
    size_t n;
    char *end = NULL;
    double value;

    if (p->pos < p->len && p->text[p->pos] == '-')
        p->pos++;

    if (p->pos >= p->len)
        return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "number ends after '-'");

    if (p->text[p->pos] == '0') {
        p->pos++;
        digits = 1;
        if (p->pos < p->len && p->text[p->pos] >= '0' && p->text[p->pos] <= '9')
            return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "number has a leading zero");
    } else {
        while (p->pos < p->len && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
            p->pos++;
            digits++;
        }
    }
    if (digits == 0)
        return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "expected a digit");

    if (p->pos < p->len && p->text[p->pos] == '.') {
        p->pos++;
        digits = 0;
        while (p->pos < p->len && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
            p->pos++;
            digits++;
        }
        if (digits == 0)
            return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "expected a digit after '.'");
    }

    if (p->pos < p->len && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->text[p->pos] == '+' || p->text[p->pos] == '-'))
            p->pos++;
        digits = 0;
        while (p->pos < p->len && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
            p->pos++;
            digits++;
        }
        if (digits == 0)
            return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "expected a digit in the exponent");
    }

    n = p->pos - start;
    if (n >= sizeof literal)
        return jp_fail(p, JSON_ERR_RANGE, start, "number literal is %lu bytes long",
                       (unsigned long)n);
    memcpy(literal, p->text + start, n);
    literal[n] = '\0';

    value = strtod(literal, &end);
    if (end != literal + n)
        return jp_fail(p, JSON_ERR_SYNTAX, start, "cannot read '%s' as a number", literal);
    /* JSON has no infinity or NaN; a literal that overflows is a bad frame, not a
     * value to pass on. The comparison form avoids pulling in <math.h>. */
    if (!(value >= -DBL_MAX && value <= DBL_MAX))
        return jp_fail(p, JSON_ERR_RANGE, start, "'%s' is not representable", literal);

    *out = value;
    return JSON_OK;
}

static int jp_literal(jparse *p, const char *word, json_type type, double number,
                      int boolean, json_value **out)
{
    size_t n = strlen(word);

    if (p->len - p->pos < n)
        return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "truncated '%s'", word);
    if (memcmp(p->text + p->pos, word, n) != 0)
        return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "expected 'null', 'true' or 'false'");
    p->pos += n;

    *out = jv_new(type);
    if (*out == NULL)
        return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for a scalar");
    (*out)->u.number = number;
    (*out)->u.boolean = boolean;
    return JSON_OK;
}

static int jp_value(jparse *p, json_value **out);

static int jp_array(jparse *p, json_value **out)
{
    json_value *v;
    int rc;

    if (p->depth + 1 >= JSON_MAX_DEPTH)
        return jp_fail(p, JSON_ERR_DEPTH, p->pos, "nesting deeper than %d", JSON_MAX_DEPTH);
    v = jv_new(JSON_ARRAY);
    if (v == NULL)
        return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for an array");

    p->pos++; /* '[' */
    p->depth++;
    jp_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ']') {
        p->pos++;
        p->depth--;
        *out = v;
        return JSON_OK;
    }

    for (;;) {
        json_value *item = NULL;
        char c;

        jp_ws(p);
        rc = jp_value(p, &item);
        if (rc != JSON_OK) {
            json_free(v);
            return rc;
        }
        rc = jv_array_push(v, item);
        if (rc != JSON_OK) {
            json_free(item);
            json_free(v);
            return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory growing an array");
        }

        jp_ws(p);
        if (p->pos >= p->len) {
            json_free(v);
            return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "array is not closed");
        }
        c = p->text[p->pos++];
        if (c == ',')
            continue;
        if (c == ']') {
            p->depth--;
            *out = v;
            return JSON_OK;
        }
        json_free(v);
        return jp_fail(p, JSON_ERR_SYNTAX, p->pos - 1,
                       "expected ',' or ']' in an array, found '%c'", c);
    }
}

static int jp_object(jparse *p, json_value **out)
{
    json_value *v;
    int rc;

    if (p->depth + 1 >= JSON_MAX_DEPTH)
        return jp_fail(p, JSON_ERR_DEPTH, p->pos, "nesting deeper than %d", JSON_MAX_DEPTH);
    v = jv_new(JSON_OBJECT);
    if (v == NULL)
        return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for an object");

    p->pos++; /* '{' */
    p->depth++;
    jp_ws(p);
    if (p->pos < p->len && p->text[p->pos] == '}') {
        p->pos++;
        p->depth--;
        *out = v;
        return JSON_OK;
    }

    for (;;) {
        char *key = NULL;
        size_t key_len = 0;
        json_value *item = NULL;
        char c;

        jp_ws(p);
        if (p->pos >= p->len) {
            json_free(v);
            return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "object is not closed");
        }
        if (p->text[p->pos] != '"') {
            json_free(v);
            return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "object member name must be a string");
        }
        rc = jp_string(p, &key, &key_len);
        if (rc != JSON_OK) {
            json_free(v);
            return rc;
        }

        jp_ws(p);
        if (p->pos >= p->len) {
            free(key);
            json_free(v);
            return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "object member has no value");
        }
        if (p->text[p->pos] != ':') {
            free(key);
            json_free(v);
            return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "expected ':' after a member name");
        }
        p->pos++;

        jp_ws(p);
        rc = jp_value(p, &item);
        if (rc != JSON_OK) {
            free(key);
            json_free(v);
            return rc;
        }
        rc = jv_object_push(v, key, key_len, item);
        if (rc != JSON_OK) {
            free(key);
            json_free(item);
            json_free(v);
            return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory growing an object");
        }

        jp_ws(p);
        if (p->pos >= p->len) {
            json_free(v);
            return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "object is not closed");
        }
        c = p->text[p->pos++];
        if (c == ',')
            continue;
        if (c == '}') {
            p->depth--;
            *out = v;
            return JSON_OK;
        }
        json_free(v);
        return jp_fail(p, JSON_ERR_SYNTAX, p->pos - 1,
                       "expected ',' or '}' in an object, found '%c'", c);
    }
}

static int jp_value(jparse *p, json_value **out)
{
    json_value *v;
    char c;
    int rc;

    *out = NULL;
    jp_ws(p);
    if (p->pos >= p->len)
        return jp_fail(p, JSON_ERR_TRUNCATED, p->len, "expected a JSON value");

    c = p->text[p->pos];
    switch (c) {
    case '{':
        return jp_object(p, out);
    case '[':
        return jp_array(p, out);
    case '"': {
        char *s = NULL;
        size_t len = 0;
        rc = jp_string(p, &s, &len);
        if (rc != JSON_OK)
            return rc;
        v = jv_new(JSON_STRING);
        if (v == NULL) {
            free(s);
            return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for a string node");
        }
        v->u.string.ptr = s;
        v->u.string.len = len;
        *out = v;
        return JSON_OK;
    }
    case 't':
        return jp_literal(p, "true", JSON_BOOL, 0.0, 1, out);
    case 'f':
        return jp_literal(p, "false", JSON_BOOL, 0.0, 0, out);
    case 'n':
        return jp_literal(p, "null", JSON_NULL, 0.0, 0, out);
    default:
        break;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        double number = 0.0;
        rc = jp_number(p, &number);
        if (rc != JSON_OK)
            return rc;
        v = jv_new(JSON_NUMBER);
        if (v == NULL)
            return jp_fail(p, JSON_ERR_NOMEM, p->pos, "out of memory for a number node");
        v->u.number = number;
        *out = v;
        return JSON_OK;
    }

    return jp_fail(p, JSON_ERR_SYNTAX, p->pos, "unexpected character '%c'", c);
}

int json_parse_ex(const char *text, size_t len, json_value **out, json_error *err)
{
    jparse p;
    json_value *v = NULL;
    int rc;

    json_error_init(err);
    if (out == NULL)
        return JSON_ERR_SYNTAX;
    *out = NULL;

    p.text = text;
    p.len = text == NULL ? 0 : len;
    p.pos = 0;
    p.depth = 0;
    p.err = err;

    if (text == NULL)
        return jp_fail(&p, JSON_ERR_TRUNCATED, 0, "no input");

    rc = jp_value(&p, &v);
    if (rc != JSON_OK)
        return rc;

    jp_ws(&p);
    if (p.pos != p.len) {
        rc = jp_fail(&p, JSON_ERR_TRAILING, p.pos, "unexpected byte '%c' after the value",
                     p.text[p.pos]);
        json_free(v);
        return rc;
    }

    *out = v;
    return JSON_OK;
}

int json_parse(const char *text, size_t len, json_value **out)
{
    return json_parse_ex(text, len, out, NULL);
}

/* ------------------------------------------------------------------ */
/* accessors                                                           */
/* ------------------------------------------------------------------ */

json_type json_typeof(const json_value *value)
{
    return value == NULL ? JSON_NULL : value->type;
}

int json_is_null(const json_value *value)
{
    return value != NULL && value->type == JSON_NULL;
}

size_t json_object_size(const json_value *value)
{
    return (value != NULL && value->type == JSON_OBJECT) ? value->u.object.count : 0;
}

const char *json_object_key_at(const json_value *value, size_t index)
{
    if (value == NULL || value->type != JSON_OBJECT || index >= value->u.object.count)
        return NULL;
    return value->u.object.members[index].key;
}

size_t json_object_key_length_at(const json_value *value, size_t index)
{
    if (value == NULL || value->type != JSON_OBJECT || index >= value->u.object.count)
        return 0;
    return value->u.object.members[index].key_len;
}

const json_value *json_object_value_at(const json_value *value, size_t index)
{
    if (value == NULL || value->type != JSON_OBJECT || index >= value->u.object.count)
        return NULL;
    return value->u.object.members[index].value;
}

const json_value *json_object_get(const json_value *value, const char *key)
{
    size_t i;

    if (value == NULL || value->type != JSON_OBJECT || key == NULL)
        return NULL;
    for (i = 0; i < value->u.object.count; i++) {
        if (strcmp(value->u.object.members[i].key, key) == 0)
            return value->u.object.members[i].value;
    }
    return NULL;
}

size_t json_array_size(const json_value *value)
{
    return (value != NULL && value->type == JSON_ARRAY) ? value->u.array.count : 0;
}

const json_value *json_array_get(const json_value *value, size_t index)
{
    if (value == NULL || value->type != JSON_ARRAY || index >= value->u.array.count)
        return NULL;
    return value->u.array.items[index];
}

const char *json_string_value(const json_value *value)
{
    if (value == NULL || value->type != JSON_STRING)
        return NULL;
    return value->u.string.ptr;
}

size_t json_string_length(const json_value *value)
{
    if (value == NULL || value->type != JSON_STRING)
        return 0;
    return value->u.string.len;
}

double json_number_value(const json_value *value)
{
    return (value != NULL && value->type == JSON_NUMBER) ? value->u.number : 0.0;
}

int json_bool_value(const json_value *value)
{
    return (value != NULL && value->type == JSON_BOOL) ? value->u.boolean : 0;
}

int json_object_get_string(const json_value *object, const char *key,
                           const char **out, size_t *out_len)
{
    const json_value *v = json_object_get(object, key);

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_STRING)
        return JSON_ERR_TYPE;
    if (out != NULL)
        *out = v->u.string.ptr;
    if (out_len != NULL)
        *out_len = v->u.string.len;
    return JSON_OK;
}

int json_object_get_int(const json_value *object, const char *key, long long *out)
{
    const json_value *v = json_object_get(object, key);
    double d;
    long long iv;

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_NUMBER)
        return JSON_ERR_TYPE;
    d = v->u.number;
    /* 2^53 is the last integer a double represents exactly; beyond it the literal
     * would silently change value, so it is reported instead. */
    if (d < -9007199254740992.0 || d > 9007199254740992.0)
        return JSON_ERR_RANGE;
    iv = (long long)d;
    if ((double)iv != d)
        return JSON_ERR_RANGE;
    if (out != NULL)
        *out = iv;
    return JSON_OK;
}

int json_object_get_double(const json_value *object, const char *key, double *out)
{
    const json_value *v = json_object_get(object, key);

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_NUMBER)
        return JSON_ERR_TYPE;
    if (out != NULL)
        *out = v->u.number;
    return JSON_OK;
}

int json_object_get_bool(const json_value *object, const char *key, int *out)
{
    const json_value *v = json_object_get(object, key);

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_BOOL)
        return JSON_ERR_TYPE;
    if (out != NULL)
        *out = v->u.boolean;
    return JSON_OK;
}

int json_object_get_object(const json_value *object, const char *key,
                           const json_value **out)
{
    const json_value *v = json_object_get(object, key);

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_OBJECT)
        return JSON_ERR_TYPE;
    if (out != NULL)
        *out = v;
    return JSON_OK;
}

int json_object_get_array(const json_value *object, const char *key,
                          const json_value **out)
{
    const json_value *v = json_object_get(object, key);

    if (v == NULL)
        return JSON_ERR_MISSING;
    if (v->type != JSON_ARRAY)
        return JSON_ERR_TYPE;
    if (out != NULL)
        *out = v;
    return JSON_OK;
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */

int json_writer_init(json_writer *writer, char *buf, size_t cap)
{
    if (writer == NULL)
        return JSON_ERR_NOSPACE;
    writer->buf = buf;
    writer->cap = cap;
    writer->len = 0;
    writer->depth = 0;
    writer->kind[0] = 0;
    writer->need_comma[0] = 0;
    writer->error = (buf == NULL || cap == 0) ? JSON_ERR_NOSPACE : JSON_OK;
    if (buf != NULL && cap > 0)
        buf[0] = '\0';
    return writer->error;
}

int json_writer_error(const json_writer *writer)
{
    return writer == NULL ? JSON_ERR_NOSPACE : writer->error;
}

size_t json_writer_length(const json_writer *writer)
{
    return writer == NULL ? 0 : writer->len;
}

const char *json_writer_text(const json_writer *writer)
{
    return writer == NULL ? NULL : writer->buf;
}

static int w_put(json_writer *w, const char *bytes, size_t len)
{
    if (w->error != JSON_OK)
        return w->error;
    /* One byte stays reserved for the NUL terminator, so the caller can always
     * treat a successful buffer as a C string. Writing the test this way keeps
     * len + 1 from wrapping around. */
    if (w->cap == 0 || len >= w->cap - w->len) {
        w->error = JSON_ERR_NOSPACE;
        return w->error;
    }
    if (len > 0)
        memcpy(w->buf + w->len, bytes, len);
    w->len += len;
    w->buf[w->len] = '\0';
    return JSON_OK;
}

static int w_putc(json_writer *w, char c)
{
    return w_put(w, &c, 1);
}

static int w_prep(json_writer *w)
{
    if (w->error != JSON_OK)
        return w->error;
    if (w->depth < 0 || w->depth >= JSON_MAX_DEPTH) {
        w->error = JSON_ERR_DEPTH;
        return w->error;
    }
    if (w->need_comma[w->depth]) {
        int rc = w_putc(w, ',');
        if (rc != JSON_OK)
            return rc;
    }
    w->need_comma[w->depth] = 1;
    return JSON_OK;
}

static int w_string(json_writer *w, const char *value, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    int rc;

    rc = w_putc(w, '"');
    if (rc != JSON_OK)
        return rc;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        switch (c) {
        case '"': rc = w_put(w, "\\\"", 2); break;
        case '\\': rc = w_put(w, "\\\\", 2); break;
        case '\b': rc = w_put(w, "\\b", 2); break;
        case '\f': rc = w_put(w, "\\f", 2); break;
        case '\n': rc = w_put(w, "\\n", 2); break;
        case '\r': rc = w_put(w, "\\r", 2); break;
        case '\t': rc = w_put(w, "\\t", 2); break;
        default:
            if (c < 0x20u) {
                char esc[6];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(c >> 4) & 0x0Fu];
                esc[5] = hex[c & 0x0Fu];
                rc = w_put(w, esc, sizeof esc);
            } else {
                rc = w_putc(w, (char)c);
            }
            break;
        }
        if (rc != JSON_OK)
            return rc;
    }
    return w_putc(w, '"');
}

int json_writer_object_begin(json_writer *w)
{
    int rc = w_prep(w);

    if (rc == JSON_OK)
        rc = w_putc(w, '{');
    if (rc != JSON_OK)
        return rc;
    if (w->depth + 1 >= JSON_MAX_DEPTH) {
        w->error = JSON_ERR_DEPTH;
        return w->error;
    }
    w->depth++;
    w->kind[w->depth] = 1;
    w->need_comma[w->depth] = 0;
    return JSON_OK;
}

int json_writer_object_end(json_writer *w)
{
    int rc;

    if (w->error != JSON_OK)
        return w->error;
    if (w->depth <= 0 || w->kind[w->depth] != 1) {
        w->error = JSON_ERR_TYPE;
        return w->error;
    }
    rc = w_putc(w, '}');
    if (rc != JSON_OK)
        return rc;
    w->depth--;
    return JSON_OK;
}

int json_writer_array_begin(json_writer *w)
{
    int rc = w_prep(w);

    if (rc == JSON_OK)
        rc = w_putc(w, '[');
    if (rc != JSON_OK)
        return rc;
    if (w->depth + 1 >= JSON_MAX_DEPTH) {
        w->error = JSON_ERR_DEPTH;
        return w->error;
    }
    w->depth++;
    w->kind[w->depth] = 0;
    w->need_comma[w->depth] = 0;
    return JSON_OK;
}

int json_writer_array_end(json_writer *w)
{
    int rc;

    if (w->error != JSON_OK)
        return w->error;
    if (w->depth <= 0 || w->kind[w->depth] != 0) {
        w->error = JSON_ERR_TYPE;
        return w->error;
    }
    rc = w_putc(w, ']');
    if (rc != JSON_OK)
        return rc;
    w->depth--;
    return JSON_OK;
}

int json_writer_key(json_writer *w, const char *key)
{
    int rc;

    if (w == NULL || key == NULL)
        return JSON_ERR_TYPE;
    rc = w_prep(w);
    if (rc != JSON_OK)
        return rc;
    if (w->depth <= 0 || w->kind[w->depth] != 1) {
        w->error = JSON_ERR_TYPE;
        return w->error;
    }
    rc = w_string(w, key, strlen(key));
    if (rc == JSON_OK)
        rc = w_putc(w, ':');
    if (rc != JSON_OK)
        return rc;
    /* The value follows without a comma; w_prep() sets the flag again. */
    w->need_comma[w->depth] = 0;
    return JSON_OK;
}

int json_writer_string(json_writer *w, const char *value)
{
    if (w == NULL || value == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    return w_string(w, value, strlen(value));
}

int json_writer_string_n(json_writer *w, const char *value, size_t len)
{
    if (w == NULL || value == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    return w_string(w, value, len);
}

/* Integral values print without a fraction so "x": 12 stays byte-identical to the
 * document's samples; small non-integral ones keep 6 decimals. The exponent form
 * is only a fallback for values far outside a kitchen's coordinate range. */
static int w_number(json_writer *w, double value)
{
    char tmp[64];
    int n;

    if (!(value >= -DBL_MAX && value <= DBL_MAX))
        return JSON_ERR_RANGE; /* NaN and ±infinity have no JSON spelling */

    if (value == 0.0) {
        return w_putc(w, '0');
    }
    if (value > -9007199254740992.0 && value < 9007199254740992.0 &&
        (double)(long long)value == value) {
        n = snprintf(tmp, sizeof tmp, "%lld", (long long)value);
    } else if (value > 1e-4 && value < 1e15) {
        size_t len;
        n = snprintf(tmp, sizeof tmp, "%.6f", value);
        if (n > 0 && (size_t)n < sizeof tmp) {
            len = (size_t)n;
            while (len > 0 && tmp[len - 1] == '0')
                tmp[--len] = '\0';
            if (len > 0 && tmp[len - 1] == '.')
                tmp[--len] = '\0';
            n = (int)len;
        }
    } else {
        n = snprintf(tmp, sizeof tmp, "%.6g", value);
    }
    if (n < 0 || (size_t)n >= sizeof tmp)
        return JSON_ERR_RANGE;
    return w_put(w, tmp, (size_t)n);
}

int json_writer_int(json_writer *w, long long value)
{
    char tmp[32];
    int n;

    if (w == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    n = snprintf(tmp, sizeof tmp, "%lld", value);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        w->error = JSON_ERR_RANGE;
        return w->error;
    }
    return w_put(w, tmp, (size_t)n);
}

int json_writer_double(json_writer *w, double value)
{
    if (w == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    {
        int rc = w_number(w, value);
        if (rc != JSON_OK)
            w->error = rc;
    }
    return w->error;
}

int json_writer_bool(json_writer *w, int value)
{
    if (w == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    return w_put(w, value ? "true" : "false", value ? 4 : 5);
}

int json_writer_null(json_writer *w)
{
    if (w == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    return w_put(w, "null", 4);
}

int json_writer_raw(json_writer *w, const char *json)
{
    if (w == NULL || json == NULL)
        return JSON_ERR_TYPE;
    if (w_prep(w) != JSON_OK)
        return w->error;
    return w_put(w, json, strlen(json));
}
