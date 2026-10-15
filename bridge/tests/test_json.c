/*
 * test_json.c -- unit tests for the JSON reader/writer.
 *
 * Covers the subset the plugin uses (objects, arrays, strings, numbers, booleans,
 * null), the escapes, and the malformed inputs the bridge must survive: a bad
 * frame may cost one message but never the process.
 */
#include <kitchenos/json.h>

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                               \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,    \
                   __LINE__, #actual, a_, e_);                               \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *a_ = (actual);                                           \
        const char *e_ = (expected);                                         \
        g_checks++;                                                          \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                             \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n",          \
                   __FILE__, __LINE__, #actual, a_ == NULL ? "(null)" : a_,  \
                   e_);                                                      \
        }                                                                    \
    } while (0)

/* Parses `text` expecting failure and reports the code it produced. */
static int parse_expect_error(const char *text)
{
    json_value *root = NULL;
    json_error err;
    int rc = json_parse_ex(text, strlen(text), &root, &err);

    if (rc == JSON_OK) {
        json_free(root);
        return JSON_OK;
    }
    if (err.code != rc) {
        printf("  FAIL json_error.code %d does not match return %d\n", err.code, rc);
        g_failures++;
    }
    if (err.detail[0] == '\0') {
        printf("  FAIL json_error.detail is empty for '%s'\n", text);
        g_failures++;
    }
    if (strcmp(json_strerror(rc), "unknown JSON error") == 0) {
        printf("  FAIL no message for code %d\n", rc);
        g_failures++;
    }
    return rc;
}

static void test_empty_containers(void)
{
    json_value *root = NULL;
    json_error err;

    CHECK_INT(json_parse_ex("{}", 2, &root, &err), JSON_OK);
    CHECK_INT(json_typeof(root), JSON_OBJECT);
    CHECK_INT(json_object_size(root), 0);
    CHECK(json_object_get(root, "anything") == NULL);
    json_free(root);

    root = NULL;
    CHECK_INT(json_parse_ex("[]", 2, &root, &err), JSON_OK);
    CHECK_INT(json_typeof(root), JSON_ARRAY);
    CHECK_INT(json_array_size(root), 0);
    CHECK(json_array_get(root, 0) == NULL);
    json_free(root);

    root = NULL;
    CHECK_INT(json_parse_ex("  {  }  ", 8, &root, &err), JSON_OK);
    CHECK_INT(json_object_size(root), 0);
    json_free(root);
}

static void test_nested(void)
{
    static const char text[] =
        "{\"a\":{\"b\":[1,2,{\"c\":3}]},\"d\":[],\"e\":[[\"deep\"]]}";
    json_value *root = NULL;
    const json_value *a;
    const json_value *b;
    const json_value *c;
    const json_value *e;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_OK);
    CHECK_INT(json_object_size(root), 3);

    a = json_object_get(root, "a");
    CHECK_INT(json_typeof(a), JSON_OBJECT);
    b = json_object_get(a, "b");
    CHECK_INT(json_typeof(b), JSON_ARRAY);
    CHECK_INT(json_array_size(b), 3);
    CHECK_INT((long long)json_number_value(json_array_get(b, 0)), 1);
    c = json_array_get(b, 2);
    CHECK_INT((long long)json_number_value(json_object_get(c, "c")), 3);
    CHECK_INT(json_array_size(json_object_get(root, "d")), 0);
    e = json_object_get(root, "e");
    CHECK_STR(json_string_value(json_array_get(json_array_get(e, 0), 0)), "deep");

    json_free(root);
}

static void test_escapes(void)
{
    static const char text[] =
        "{\"s\":\"line\\nbreak\\ttab\\\"quote\\\"\\\\back\\/slash\","
        "\"cjk\":\"\\u4e2d\\u6587\","
        "\"emoji\":\"\\ud83d\\ude00\","
        "\"letter\":\"\\u0041\","
        "\"nul\":\"a\\u0000b\","
        "\"raw\":\"caf\xc3\xa9\"}";
    json_value *root = NULL;
    const char *value = NULL;
    size_t len = 0;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_OK);

    CHECK_INT(json_object_get_string(root, "s", &value, &len), JSON_OK);
    CHECK_STR(value, "line\nbreak\ttab\"quote\"\\back/slash");
    CHECK_INT(len, strlen("line\nbreak\ttab\"quote\"\\back/slash"));

    CHECK_INT(json_object_get_string(root, "cjk", &value, &len), JSON_OK);
    CHECK_STR(value, "\xe4\xb8\xad\xe6\x96\x87"); /* U+4E2D U+6587 */
    CHECK_INT(len, 6);

    CHECK_INT(json_object_get_string(root, "emoji", &value, &len), JSON_OK);
    CHECK_STR(value, "\xf0\x9f\x98\x80"); /* U+1F600, surrogate pair */
    CHECK_INT(len, 4);

    CHECK_INT(json_object_get_string(root, "letter", &value, &len), JSON_OK);
    CHECK_STR(value, "A");

    /* Embedded NULs survive; the length is what makes the string usable. */
    CHECK_INT(json_object_get_string(root, "nul", &value, &len), JSON_OK);
    CHECK_INT(len, 3);
    CHECK_INT((unsigned char)value[0], 'a');
    CHECK_INT((unsigned char)value[1], 0);
    CHECK_INT((unsigned char)value[2], 'b');

    CHECK_INT(json_object_get_string(root, "raw", &value, &len), JSON_OK);
    CHECK_STR(value, "caf\xc3\xa9");

    json_free(root);
}

static void test_numbers(void)
{
    static const char text[] =
        "{\"i\":42,\"n\":-17,\"d\":3.5,\"z\":0,\"nz\":-0,"
        "\"e\":1e3,\"ne\":-2.5e-1,\"big\":1234567890,\"ts\":1234567890}";
    json_value *root = NULL;
    double d = 0.0;
    long long i = 0;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_OK);

    CHECK_INT(json_object_get_int(root, "i", &i), JSON_OK);
    CHECK_INT(i, 42);
    CHECK_INT(json_object_get_int(root, "n", &i), JSON_OK);
    CHECK_INT(i, -17);
    CHECK_INT(json_object_get_int(root, "big", &i), JSON_OK);
    CHECK_INT(i, 1234567890LL);
    CHECK_INT(json_object_get_int(root, "ts", &i), JSON_OK);
    CHECK_INT(i, 1234567890LL);
    CHECK_INT(json_object_get_int(root, "z", &i), JSON_OK);
    CHECK_INT(i, 0);
    CHECK_INT(json_object_get_int(root, "nz", &i), JSON_OK);
    CHECK_INT(i, 0);

    CHECK_INT(json_object_get_double(root, "d", &d), JSON_OK);
    CHECK(d == 3.5);
    CHECK_INT(json_object_get_double(root, "e", &d), JSON_OK);
    CHECK(d == 1000.0);
    CHECK_INT(json_object_get_double(root, "ne", &d), JSON_OK);
    CHECK(d == -0.25);

    /* A decimal is not an integer: asking for one reports rather than rounds. */
    CHECK_INT(json_object_get_int(root, "d", &i), JSON_ERR_RANGE);

    json_free(root);
}

static void test_literals(void)
{
    static const char text[] = "{\"t\":true,\"f\":false,\"n\":null}";
    json_value *root = NULL;
    int b = -1;
    long long i = 0;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_OK);
    CHECK_INT(json_typeof(json_object_get(root, "t")), JSON_BOOL);
    CHECK_INT(json_object_get_bool(root, "t", &b), JSON_OK);
    CHECK_INT(b, 1);
    CHECK_INT(json_object_get_bool(root, "f", &b), JSON_OK);
    CHECK_INT(b, 0);
    CHECK(json_is_null(json_object_get(root, "n")));
    CHECK_INT(json_object_get_bool(root, "n", &b), JSON_ERR_TYPE);
    /* true is not a number, even though it is spelled with letters. */
    CHECK_INT(json_object_get_int(root, "t", &i), JSON_ERR_TYPE);
    json_free(root);
}

static void test_lookup_errors(void)
{
    static const char text[] = "{\"s\":\"x\",\"n\":5}";
    json_value *root = NULL;
    const char *s = NULL;
    size_t len = 0;
    long long i = 0;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_OK);
    CHECK_INT(json_object_get_string(root, "absent", &s, &len), JSON_ERR_MISSING);
    CHECK_INT(json_object_get_string(root, "n", &s, &len), JSON_ERR_TYPE);
    CHECK_INT(json_object_get_int(root, "absent", &i), JSON_ERR_MISSING);
    CHECK_INT(json_object_get_int(root, "s", &i), JSON_ERR_TYPE);
    /* Accessors on a non-object are simply empty, never a crash. */
    CHECK_INT(json_object_size(json_object_get(root, "n")), 0);
    CHECK(json_object_get(root, "n") != NULL);
    json_free(root);

    json_free(NULL); /* documented no-op */
}

static void test_malformed(void)
{
    /* truncated */
    CHECK_INT(parse_expect_error("{\"a\":"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("{\"a\":1"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("[1,2"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error(""), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("\""), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("tru"), JSON_ERR_TRUNCATED);

    /* trailing garbage after a complete value */
    CHECK_INT(parse_expect_error("{} x"), JSON_ERR_TRAILING);
    CHECK_INT(parse_expect_error("{\"a\":1}{}"), JSON_ERR_TRAILING);
    CHECK_INT(parse_expect_error("[1] [2]"), JSON_ERR_TRAILING);

    /* structural mistakes */
    CHECK_INT(parse_expect_error("{\"a\":1,}"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("[1,]"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("[,1]"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("{a:1}"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("{\"a\" 1}"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("{'a':1}"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("[1 2]"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("nul"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("nope"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("@"), JSON_ERR_SYNTAX);

    /* bad escapes */
    CHECK_INT(parse_expect_error("\"\\q\""), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("\"\\u00zz\""), JSON_ERR_UTF8);
    CHECK_INT(parse_expect_error("\"\\u00\""), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("\"\\ud800\""), JSON_ERR_UTF8);       /* lone high */
    CHECK_INT(parse_expect_error("\"\\udc00\""), JSON_ERR_UTF8);       /* lone low */
    CHECK_INT(parse_expect_error("\"\\ud800\\u0041\""), JSON_ERR_UTF8); /* bad pair */
    CHECK_INT(parse_expect_error("\"unterminated"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("\"raw\ttab\""), JSON_ERR_SYNTAX); /* unescaped control */

    /* bad numbers */
    CHECK_INT(parse_expect_error("01"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("-"), JSON_ERR_TRUNCATED);
    CHECK_INT(parse_expect_error("1."), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("1e"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error(".5"), JSON_ERR_SYNTAX);
    CHECK_INT(parse_expect_error("1e999"), JSON_ERR_RANGE);
}

static void test_error_context(void)
{
    static const char text[] = "{\n  \"a\": 1,\n  \"b\": @\n}";
    json_value *root = NULL;
    json_error err;

    CHECK_INT(json_parse_ex(text, sizeof text - 1, &root, &err), JSON_ERR_SYNTAX);
    CHECK(root == NULL);
    CHECK_INT(err.code, JSON_ERR_SYNTAX);
    CHECK_INT(err.line, 3);
    CHECK_INT(err.column, 8);
    CHECK_STR(err.detail, "unexpected character '@'");
}

static void test_depth_limit(void)
{
    char text[512];
    json_value *root = NULL;
    json_error err;
    size_t i;
    size_t n = 100;

    for (i = 0; i < n; i++)
        text[i] = '[';
    for (i = 0; i < n; i++)
        text[n + i] = ']';
    CHECK_INT(json_parse_ex(text, 2 * n, &root, &err), JSON_ERR_DEPTH);
    CHECK_INT(err.code, JSON_ERR_DEPTH);

    /* Ten levels are well inside the limit. */
    n = 10;
    for (i = 0; i < n; i++)
        text[i] = '[';
    for (i = 0; i < n; i++)
        text[n + i] = ']';
    CHECK_INT(json_parse_ex(text, 2 * n, &root, &err), JSON_OK);
    CHECK_INT(json_typeof(root), JSON_ARRAY);
    json_free(root);
}

static void test_writer(void)
{
    char buf[256];
    json_writer w;

    CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
    json_writer_object_begin(&w);
    json_writer_key(&w, "a");
    json_writer_int(&w, 1);
    json_writer_key(&w, "b");
    json_writer_array_begin(&w);
    json_writer_string(&w, "x");
    json_writer_bool(&w, 1);
    json_writer_bool(&w, 0);
    json_writer_null(&w);
    json_writer_double(&w, 2.5);
    json_writer_array_end(&w);
    json_writer_key(&w, "empty");
    json_writer_object_begin(&w);
    json_writer_object_end(&w);
    json_writer_object_end(&w);
    CHECK_INT(json_writer_error(&w), JSON_OK);
    CHECK_STR(buf, "{\"a\":1,\"b\":[\"x\",true,false,null,2.5],\"empty\":{}}");
    CHECK_INT(json_writer_length(&w), strlen(buf));
    CHECK(json_writer_text(&w) == buf);

    /* Escaping on the way out, including a control byte. The \x01 is split from
     * the following "f" so the compiler does not read \x01f as one hex escape. */
    CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
    json_writer_object_begin(&w);
    json_writer_key(&w, "k\ney");
    json_writer_string(&w, "a\"b\\c\nd\te\x01" "f");
    json_writer_object_end(&w);
    CHECK_INT(json_writer_error(&w), JSON_OK);
    CHECK_STR(buf, "{\"k\\ney\":\"a\\\"b\\\\c\\nd\\te\\u0001f\"}");
}

static void test_writer_numbers(void)
{
    char buf[128];
    json_writer w;

    CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
    json_writer_array_begin(&w);
    json_writer_double(&w, 12.0);   /* integral prints without a fraction */
    json_writer_double(&w, -0.5);
    json_writer_double(&w, 10.5);
    json_writer_double(&w, 0.0);
    json_writer_double(&w, -0.0);
    json_writer_double(&w, 0.25);
    json_writer_int(&w, -7);
    json_writer_array_end(&w);
    CHECK_INT(json_writer_error(&w), JSON_OK);
    CHECK_STR(buf, "[12,-0.5,10.5,0,0,0.25,-7]");

    /* Every number the writer produces must parse back to the same value. */
    {
        json_value *root = NULL;
        json_error err;
        size_t i;
        static const double values[] = { 0.0, 1.0, -1.5, 10.5, 1234.25, -0.125 };

        for (i = 0; i < sizeof values / sizeof values[0]; i++) {
            CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
            json_writer_double(&w, values[i]);
            CHECK_INT(json_writer_error(&w), JSON_OK);
            CHECK_INT(json_parse_ex(buf, strlen(buf), &root, &err), JSON_OK);
            CHECK(json_number_value(root) == values[i]);
            json_free(root);
            root = NULL;
        }
    }
}

static void test_writer_failures(void)
{
    char small[8];
    char buf[32];
    json_writer w;

    /* Too small for the message: the error sticks and the buffer is discarded. */
    CHECK_INT(json_writer_init(&w, small, sizeof small), JSON_OK);
    json_writer_object_begin(&w);
    json_writer_key(&w, "key");
    json_writer_string(&w, "value that cannot fit");
    CHECK_INT(json_writer_error(&w), JSON_ERR_NOSPACE);
    CHECK_INT(json_writer_object_end(&w), JSON_ERR_NOSPACE);

    /* Ending the wrong container is a caller bug, not a buffer problem. */
    CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
    json_writer_object_begin(&w);
    CHECK_INT(json_writer_array_end(&w), JSON_ERR_TYPE);
    CHECK_INT(json_writer_error(&w), JSON_ERR_TYPE);

    /* Value at the top level with no room for the NUL terminator. */
    CHECK_INT(json_writer_init(&w, buf, 2), JSON_OK);
    json_writer_string(&w, "ab");
    CHECK_INT(json_writer_error(&w), JSON_ERR_NOSPACE);

    CHECK_INT(json_writer_init(&w, NULL, 0), JSON_ERR_NOSPACE);
    CHECK_INT(json_writer_error(&w), JSON_ERR_NOSPACE);
}

static void test_round_trip(void)
{
    char buf[512];
    json_writer w;
    json_value *root = NULL;
    json_error err;

    CHECK_INT(json_writer_init(&w, buf, sizeof buf), JSON_OK);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, "state_update");
    json_writer_key(&w, "nested");
    json_writer_object_begin(&w);
    json_writer_key(&w, "list");
    json_writer_array_begin(&w);
    json_writer_int(&w, 1);
    json_writer_int(&w, 2);
    json_writer_array_end(&w);
    json_writer_object_end(&w);
    json_writer_object_end(&w);

    CHECK_INT(json_parse_ex(buf, json_writer_length(&w), &root, &err), JSON_OK);
    CHECK_STR(json_string_value(json_object_get(root, "type")), "state_update");
    CHECK_INT(json_array_size(json_object_get(json_object_get(root, "nested"), "list")), 2);
    json_free(root);
}

int main(void)
{
    printf("test_json: JSON parser and writer\n");

    test_empty_containers();
    test_nested();
    test_escapes();
    test_numbers();
    test_literals();
    test_lookup_errors();
    test_malformed();
    test_error_context();
    test_depth_limit();
    test_writer();
    test_writer_numbers();
    test_writer_failures();
    test_round_trip();

    printf("test_json: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
