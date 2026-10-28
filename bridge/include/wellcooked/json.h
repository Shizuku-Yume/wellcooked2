/*
 * json.h -- hand-written JSON reader/writer for the WellCooked bridge.
 *
 * The subset is exactly what the game plugin emits and accepts: objects,
 * arrays, strings, numbers, booleans and null. No comments, no trailing commas.
 * Everything returns a status code plus, for the parser, a json_error record with
 * the byte offset; a malformed frame from the game costs one message, never the
 * process.
 *
 * The parser keeps the whole document in memory (no streaming): a frame is at
 * most 1 MiB, and protocol.c needs random access to fields anyway.
 */
#ifndef WELLCOOKED_JSON_H
#define WELLCOOKED_JSON_H

#include <stddef.h>

/* Nesting beyond this is a parse error: it only shows up in garbage input, and
 * the recursive-descent parser would otherwise be a stack overflow vector. */
#define JSON_MAX_DEPTH 64

/* Status codes. Zero is success; every failure is negative so callers can test
 * `if (rc < 0)`. Each module keeps its own code space. */
enum {
    JSON_OK = 0,
    JSON_ERR_SYNTAX = -1,    /* malformed JSON */
    JSON_ERR_TRUNCATED = -2, /* input ended in the middle of a value */
    JSON_ERR_TRAILING = -3,  /* extra bytes after the top-level value */
    JSON_ERR_DEPTH = -4,     /* nesting deeper than JSON_MAX_DEPTH */
    JSON_ERR_UTF8 = -5,      /* bad \u escape: bad hex, lone or wrong surrogate */
    JSON_ERR_NOMEM = -6,     /* allocation failed (errno is recorded) */
    JSON_ERR_NOSPACE = -7,   /* caller buffer too small */
    JSON_ERR_RANGE = -8,     /* number literal not representable / not integral */
    JSON_ERR_TYPE = -9,      /* accessor used on a value of another type */
    JSON_ERR_MISSING = -10   /* object has no member with that name */
};

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

/* Opaque document node. */
typedef struct json_value json_value;

/* Context for one failed parse. `detail` is a human-readable sentence for the
 * bridge log; the offset/line/column say where in the frame it went wrong. */
typedef struct {
    int code;        /* JSON_OK while parsing is still successful */
    size_t offset;   /* byte offset into the parsed text */
    int line;        /* 1-based, counted from the start of the text */
    int column;      /* 1-based */
    int sys_errno;   /* errno of the failing call when the code came from the OS */
    char detail[128];
} json_error;

void json_error_init(json_error *err);
const char *json_strerror(int code);

/* Parse a complete JSON document. On success *out owns the document and must be
 * released with json_free(); on failure *out is NULL and `err` (optional) says
 * why. `len` excludes any NUL terminator: frames carry their length explicitly
 * and may contain embedded NUL bytes. */
int json_parse(const char *text, size_t len, json_value **out);
int json_parse_ex(const char *text, size_t len, json_value **out, json_error *err);
void json_free(json_value *value);

/* ---- generic read accessors (all tolerate NULL) ---- */

json_type json_typeof(const json_value *value);
int json_is_null(const json_value *value);

size_t json_object_size(const json_value *value);
const char *json_object_key_at(const json_value *value, size_t index);
size_t json_object_key_length_at(const json_value *value, size_t index);
const json_value *json_object_value_at(const json_value *value, size_t index);
/* Members keep their document order; with duplicate names the first one wins. */
const json_value *json_object_get(const json_value *value, const char *key);

size_t json_array_size(const json_value *value);
const json_value *json_array_get(const json_value *value, size_t index);

/* Strings are stored NUL-terminated, but may contain embedded NULs, so the
 * length is reported separately. Both return NULL/0 for non-strings. */
const char *json_string_value(const json_value *value);
size_t json_string_length(const json_value *value);

double json_number_value(const json_value *value); /* 0.0 for non-numbers */
int json_bool_value(const json_value *value);      /* 0 for non-booleans */

/* ---- typed member lookup ---- */

/* Pointers returned through `out` stay valid until json_free() of the document
 * they came from. Absent member -> JSON_ERR_MISSING, present but of another JSON
 * type -> JSON_ERR_TYPE, non-integral or too large -> JSON_ERR_RANGE. */
int json_object_get_string(const json_value *object, const char *key,
                           const char **out, size_t *out_len);
int json_object_get_int(const json_value *object, const char *key, long long *out);
int json_object_get_double(const json_value *object, const char *key, double *out);
int json_object_get_bool(const json_value *object, const char *key, int *out);
int json_object_get_object(const json_value *object, const char *key,
                           const json_value **out);
int json_object_get_array(const json_value *object, const char *key,
                          const json_value **out);

/* ---- writer ----
 *
 * The writer formats straight into a caller-owned buffer: no allocation, so it is
 * safe to call at frame rate, and overflow is a returned code instead of a crash.
 * The first failure is sticky -- later calls keep returning it and change nothing
 * -- so a builder can run to the end and check once. On success the text is
 * NUL-terminated; on failure the buffer holds an unusable partial message.
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int error;
    int depth;
    unsigned char kind[JSON_MAX_DEPTH];       /* 1 = object, 0 = array */
    unsigned char need_comma[JSON_MAX_DEPTH];
} json_writer;

int json_writer_init(json_writer *writer, char *buf, size_t cap);
int json_writer_error(const json_writer *writer);
size_t json_writer_length(const json_writer *writer);
const char *json_writer_text(const json_writer *writer);

int json_writer_object_begin(json_writer *writer);
int json_writer_object_end(json_writer *writer);
int json_writer_array_begin(json_writer *writer);
int json_writer_array_end(json_writer *writer);
int json_writer_key(json_writer *writer, const char *key);

int json_writer_string(json_writer *writer, const char *value);
int json_writer_string_n(json_writer *writer, const char *value, size_t len);
int json_writer_int(json_writer *writer, long long value);
int json_writer_double(json_writer *writer, double value);
int json_writer_bool(json_writer *writer, int value);
int json_writer_null(json_writer *writer);
/* Splices already-serialized JSON (used for pre-built nested fragments). */
int json_writer_raw(json_writer *writer, const char *json);

#endif /* WELLCOOKED_JSON_H */
