/*
 * test_framing.c -- unit tests for the 4-byte big-endian length prefix framing.
 *
 * The point of these cases is TCP: bytes arrive in arbitrary chunks, so partial
 * frames must be buffered, several frames may arrive at once, and a length out of
 * range must be reported instead of desynchronizing the stream silently.
 */
#include <kitchenos/framing.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Pulls exactly one frame and compares it with what was expected. */
static void expect_frame(framing_decoder *d, const char *want, size_t want_len)
{
    const char *payload = NULL;
    size_t len = 0;
    int rc = framing_decoder_next(d, &payload, &len);

    CHECK_INT(rc, FRAMING_OK);
    if (rc != FRAMING_OK)
        return;
    CHECK_INT(len, want_len);
    CHECK(len == want_len && memcmp(payload, want, want_len) == 0);
}

static void expect_need_more(framing_decoder *d)
{
    const char *payload = NULL;
    size_t len = 0;

    CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_NEED_MORE);
}

static void test_header_layout(void)
{
    unsigned char out[16];
    unsigned char header[FRAMING_HEADER_LEN];
    size_t written = 0;
    size_t len = 0;

    CHECK_INT(framing_encoded_size(5), 9);
    CHECK_INT(framing_encode("hello", 5, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(written, 9);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 5);
    CHECK(memcmp(out + 4, "hello", 5) == 0);

    /* 258 must go out big-endian: 0x00 0x00 0x01 0x02. */
    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = 0x02;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_OK);
    CHECK_INT(len, 258);

    CHECK_INT(framing_encode("", 0, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(written, 4);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
}

static void test_single_frame(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char frame[64];
    size_t written = 0;

    CHECK(d != NULL);
    CHECK_INT(framing_encode("{\"type\":\"heartbeat\"}", 20, frame, sizeof frame,
                             &written),
              FRAMING_OK);
    CHECK_INT(framing_decoder_buffered(d), 0);
    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    CHECK_INT(framing_decoder_buffered(d), 24);
    expect_frame(d, "{\"type\":\"heartbeat\"}", 20);
    CHECK_INT(framing_decoder_buffered(d), 0);
    expect_need_more(d);

    framing_decoder_free(d);
}

static void test_split_across_reads(void)
{
    framing_decoder *d = framing_decoder_new();
    const char *message = "{\"type\":\"hello\"}";
    unsigned char frame[64];
    size_t written = 0;
    size_t i;

    CHECK_INT(framing_encode(message, strlen(message), frame, sizeof frame, &written),
              FRAMING_OK);

    /* One byte per read: the header itself arrives in pieces. */
    for (i = 0; i + 1 < written; i++) {
        CHECK_INT(framing_decoder_feed(d, frame + i, 1), FRAMING_OK);
        expect_need_more(d);
    }
    CHECK_INT(framing_decoder_feed(d, frame + written - 1, 1), FRAMING_OK);
    expect_frame(d, message, strlen(message));
    expect_need_more(d);

    /* Two reads: first the whole header, then the payload. */
    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_feed(d, frame, FRAMING_HEADER_LEN), FRAMING_OK);
    expect_need_more(d);
    CHECK_INT(framing_decoder_feed(d, frame + FRAMING_HEADER_LEN,
                                   written - FRAMING_HEADER_LEN),
              FRAMING_OK);
    expect_frame(d, message, strlen(message));

    framing_decoder_free(d);
}

static void test_two_frames_one_buffer(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char buf[256];
    size_t used = 0;
    size_t written = 0;

    CHECK_INT(framing_encode("first", 5, buf, sizeof buf, &written), FRAMING_OK);
    used += written;
    CHECK_INT(framing_encode("second", 6, buf + used, sizeof buf - used, &written),
              FRAMING_OK);
    used += written;
    /* A third frame that is only half here. */
    CHECK_INT(framing_encode("third", 5, buf + used, sizeof buf - used, &written),
              FRAMING_OK);
    CHECK_INT(framing_decoder_feed(d, buf, used + 6), FRAMING_OK); /* header + 2 bytes */

    expect_frame(d, "first", 5);
    expect_frame(d, "second", 6);
    expect_need_more(d);

    CHECK_INT(framing_decoder_feed(d, buf + used + 6, written - 6), FRAMING_OK);
    expect_frame(d, "third", 5);
    expect_need_more(d);

    framing_decoder_free(d);
}

static void test_zero_length_frame(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char buf[64];
    size_t written = 0;
    size_t used = 0;

    /* Legal at this layer: the length prefix says 0. json/protocol reject the
     * empty payload one level up, which is where it becomes a parse error. */
    CHECK_INT(framing_encode("", 0, buf, sizeof buf, &written), FRAMING_OK);
    used += written;
    CHECK_INT(framing_encode("after", 5, buf + used, sizeof buf - used, &written),
              FRAMING_OK);
    used += written;
    CHECK_INT(framing_decoder_feed(d, buf, used), FRAMING_OK);

    expect_frame(d, "", 0);
    expect_frame(d, "after", 5);
    expect_need_more(d);

    framing_decoder_free(d);
}

static void test_length_limits(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char header[FRAMING_HEADER_LEN];
    size_t len = 0;

    /* 1 MiB exactly is the last accepted length. */
    header[0] = 0x00;
    header[1] = 0x10;
    header[2] = 0x00;
    header[3] = 0x00;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_OK);
    CHECK_INT(len, FRAMING_MAX_PAYLOAD);

    /* One byte more is a framing error. */
    header[0] = 0x00;
    header[1] = 0x10;
    header[2] = 0x00;
    header[3] = 0x01;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_ERR_LENGTH);

    /* 0xFFFFFFFF is what a signed -1 looks like on the wire; TcpLink reads the
     * prefix as a signed int, so both ends must refuse it. */
    header[0] = 0xFF;
    header[1] = 0xFF;
    header[2] = 0xFF;
    header[3] = 0xFF;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_ERR_LENGTH);

    /* 0x80000000 is negative as well, and 8 MiB is simply too big. */
    header[0] = 0x80;
    header[1] = 0x00;
    header[2] = 0x00;
    header[3] = 0x00;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_ERR_LENGTH);
    header[0] = 0x00;
    header[1] = 0x80;
    header[2] = 0x00;
    header[3] = 0x00;
    CHECK_INT(framing_peek_length(header, &len), FRAMING_ERR_LENGTH);

    /* The decoder must report it rather than wait for 4 GiB to arrive. */
    header[0] = 0x00;
    header[1] = 0x10;
    header[2] = 0x00;
    header[3] = 0x01;
    CHECK_INT(framing_decoder_feed(d, header, FRAMING_HEADER_LEN), FRAMING_OK);
    {
        const char *payload = NULL;
        CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_ERR_LENGTH);
        /* Sticky: the stream is out of sync, so it stays an error until reset. */
        CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_ERR_LENGTH);
        CHECK_INT(framing_decoder_feed(d, "x", 1), FRAMING_ERR_LENGTH);
    }

    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_buffered(d), 0);
    {
        const char *payload = NULL;
        CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_NEED_MORE);
    }

    framing_decoder_free(d);
}

static void test_one_megabyte_round_trip(void)
{
    framing_decoder *d = framing_decoder_new();
    size_t size = FRAMING_MAX_PAYLOAD;
    unsigned char *payload = malloc(size);
    unsigned char *frame = malloc(size + FRAMING_HEADER_LEN);
    const char *out = NULL;
    size_t out_len = 0;
    size_t written = 0;

    CHECK(payload != NULL && frame != NULL);
    if (payload == NULL || frame == NULL) {
        free(payload);
        free(frame);
        framing_decoder_free(d);
        return;
    }
    memset(payload, 'x', size);
    payload[0] = '{';
    payload[size - 1] = '}';

    CHECK_INT(framing_encoded_size(size), size + FRAMING_HEADER_LEN);
    CHECK_INT(framing_encode(payload, size, frame, size + FRAMING_HEADER_LEN, &written),
              FRAMING_OK);
    CHECK_INT(written, size + FRAMING_HEADER_LEN);
    CHECK(frame[4] == '{' && frame[FRAMING_HEADER_LEN + size - 1] == '}');

    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &out, &out_len), FRAMING_OK);
    CHECK_INT(out_len, size);
    CHECK(memcmp(out, payload, size) == 0);

    /* One byte over the limit cannot even be encoded. */
    CHECK_INT(framing_encode(payload, size + 1, frame, size + FRAMING_HEADER_LEN, &written),
              FRAMING_ERR_LENGTH);
    CHECK_INT(framing_encoded_size(size + 1), 0);

    free(payload);
    free(frame);
    framing_decoder_free(d);
}

static void test_encode_rejects_small_buffers(void)
{
    unsigned char out[16];
    size_t written = 123;

    /* out_cap is the only length the encoder knows, so it must be the real one. */
    CHECK_INT(framing_encode("hello", 5, out, 8, &written), FRAMING_ERR_SPACE);
    CHECK_INT(written, 123); /* untouched on failure */
    CHECK_INT(framing_encode("hello", 5, out, 9, &written), FRAMING_OK);
    CHECK_INT(written, 9);
    CHECK(memcmp(out + 4, "hello", 5) == 0);
    CHECK_INT(framing_encode("hello", 5, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(framing_encode("hello", 5, NULL, 0, NULL), FRAMING_ERR_SPACE);
}

static void test_binary_safe_payload(void)
{
    framing_decoder *d = framing_decoder_new();
    static const char payload[] = "a\0b\xc3\xa9\xff"; /* NUL and non-ASCII inside */
    unsigned char frame[32];
    size_t written = 0;
    const char *out = NULL;
    size_t out_len = 0;

    CHECK_INT(framing_encode(payload, sizeof payload, frame, sizeof frame, &written),
              FRAMING_OK);
    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &out, &out_len), FRAMING_OK);
    CHECK_INT(out_len, sizeof payload);
    CHECK(memcmp(out, payload, sizeof payload) == 0);

    framing_decoder_free(d);
}

static void test_decoder_reuse(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char frame[32];
    size_t written = 0;

    /* Alternating partial feeds and drains must not lose or duplicate bytes:
     * this is the shape of a real socket loop. */
    CHECK_INT(framing_encode("alpha", 5, frame, sizeof frame, &written), FRAMING_OK);
    CHECK_INT(framing_decoder_feed(d, frame, 3), FRAMING_OK);
    expect_need_more(d);
    CHECK_INT(framing_decoder_feed(d, frame + 3, written - 3), FRAMING_OK);
    expect_frame(d, "alpha", 5);

    CHECK_INT(framing_encode("beta", 4, frame, sizeof frame, &written), FRAMING_OK);
    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    expect_frame(d, "beta", 4);
    expect_need_more(d);
    CHECK_INT(framing_decoder_buffered(d), 0);

    /* Feeding nothing is not an error. */
    CHECK_INT(framing_decoder_feed(d, frame, 0), FRAMING_OK);
    expect_need_more(d);

    /* A NULL buffer with a positive length is a caller bug; the decoder reports
     * the reason it recorded instead of only "no memory". */
    CHECK_INT(framing_decoder_errno(d), 0);
    CHECK_INT(framing_decoder_feed(d, NULL, 4), FRAMING_ERR_NOMEM);
    CHECK_INT(framing_decoder_errno(d), EINVAL);
    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_errno(d), 0);

    framing_decoder_free(d);
}

int main(void)
{
    printf("test_framing: length-prefixed stream framing\n");

    test_header_layout();
    test_single_frame();
    test_split_across_reads();
    test_two_frames_one_buffer();
    test_zero_length_frame();
    test_length_limits();
    test_one_megabyte_round_trip();
    test_encode_rejects_small_buffers();
    test_binary_safe_payload();
    test_decoder_reuse();

    printf("test_framing: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
