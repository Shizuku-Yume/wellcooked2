/*
 * test_framing.c -- unit tests for the line framing: one JSON message per line,
 * UTF-8, '\n' at the end.
 *
 * The point of these cases is TCP: bytes arrive in arbitrary chunks, so partial
 * lines must be buffered, several lines may arrive at once, and a line that
 * cannot be legal must be reported instead of desynchronizing the stream
 * silently. The two edges that are easy to regress are named in the task
 * history: a zero-length line is a *message* problem and must not cost the link
 * (eaed4f5), and a payload carrying a raw terminator must never reach the wire
 * as two messages.
 */
#include <wellcooked/framing.h>

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

/* Pulls exactly one line and compares it with what was expected. */
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

/* The encoder is the only thing that decides what a line looks like, so the
 * layout is checked against bytes on the wire and not against itself. */
static void test_line_layout(void)
{
    unsigned char out[16];
    size_t written = 0;

    CHECK_INT(framing_encoded_size(5), 6);
    CHECK_INT(framing_encode("hello", 5, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(written, 6);
    CHECK(out[0] == 'h'); /* no prefix: the payload starts at byte 0 */
    CHECK(memcmp(out, "hello", 5) == 0);
    CHECK(out[5] == FRAMING_TERMINATOR);

    /* An empty message is one terminator, not zero bytes: a reader that sees
     * nothing at all cannot tell "empty message" from "no message yet". */
    CHECK_INT(framing_encoded_size(0), 1);
    CHECK_INT(framing_encode("", 0, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(written, 1);
    CHECK(out[0] == FRAMING_TERMINATOR);

    /* A non-ASCII payload is counted in bytes, not characters. */
    CHECK_INT(framing_encoded_size(strlen("订单")), 7);
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
    CHECK_INT(framing_decoder_buffered(d), 21); /* payload + terminator */
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

    /* One byte per read. The last byte is the terminator: until it arrives the
     * decoder must not hand anything out, however complete the JSON looks. */
    for (i = 0; i + 1 < written; i++) {
        CHECK_INT(framing_decoder_feed(d, frame + i, 1), FRAMING_OK);
        expect_need_more(d);
    }
    CHECK_INT(framing_decoder_feed(d, frame + written - 1, 1), FRAMING_OK);
    expect_frame(d, message, strlen(message));
    expect_need_more(d);

    /* Two reads, this time split *at* the terminator: the whole payload arrives
     * first, then the one byte that makes it a message. */
    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_feed(d, frame, written - FRAMING_TERMINATOR_LEN),
              FRAMING_OK);
    expect_need_more(d);
    CHECK_INT(framing_decoder_feed(d, frame + written - FRAMING_TERMINATOR_LEN,
                                   FRAMING_TERMINATOR_LEN),
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
    /* A third line that is only half here. */
    CHECK_INT(framing_encode("third", 5, buf + used, sizeof buf - used, &written),
              FRAMING_OK);
    /* Feed everything except the last 3 bytes of the third line. */
    CHECK_INT(framing_decoder_feed(d, buf, used + written - 3), FRAMING_OK);

    expect_frame(d, "first", 5);
    expect_frame(d, "second", 6);
    expect_need_more(d);

    CHECK_INT(framing_decoder_feed(d, buf + used + written - 3, 3), FRAMING_OK);
    expect_frame(d, "third", 5);
    expect_need_more(d);

    framing_decoder_free(d);
}

/* eaed4f5: an empty line is a legal frame with an empty payload. The parse error
 * belongs to the caller's JSON layer, which is why the link must stay up -- and
 * why this layer must not skip the empty line, which would silently eat the
 * message that the parser is supposed to log. */
static void test_zero_length_line(void)
{
    framing_decoder *d = framing_decoder_new();
    static const char stream[] = "\n\nafter\n";

    CHECK_INT(framing_decoder_feed(d, stream, sizeof stream - 1), FRAMING_OK);
    expect_frame(d, "", 0);
    expect_frame(d, "", 0);
    expect_frame(d, "after", 5);
    expect_need_more(d);
    CHECK_INT(framing_decoder_buffered(d), 0);

    /* The same through the encoder: "" becomes exactly one terminator, and the
     * decoder hands it back as an empty payload, not as "nothing yet". */
    {
        unsigned char frame[8];
        size_t written = 0;
        const char *payload = NULL;
        size_t len = 123;

        framing_decoder_reset(d);
        CHECK_INT(framing_encode("", 0, frame, sizeof frame, &written), FRAMING_OK);
        CHECK_INT(written, 1);
        CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
        CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_OK);
        CHECK_INT(len, 0);
        CHECK(payload != NULL && payload[0] == '\n'); /* points at the terminator */
        expect_need_more(d);
    }

    framing_decoder_free(d);
}

static void test_line_length_limit(void)
{
    framing_decoder *d = framing_decoder_new();
    unsigned char *line = malloc(FRAMING_MAX_FRAME + 2);
    const char *payload = NULL;
    size_t len = 0;
    size_t i;

    CHECK(line != NULL);
    if (line == NULL) {
        framing_decoder_free(d);
        return;
    }
    memset(line, 'x', FRAMING_MAX_FRAME + 2);

    /* The cap is on the payload, so exactly FRAMING_MAX_PAYLOAD bytes followed by
     * a terminator is the last line that is still legal. */
    line[FRAMING_MAX_PAYLOAD] = FRAMING_TERMINATOR;
    CHECK_INT(framing_decoder_feed(d, line, FRAMING_MAX_FRAME), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_OK);
    CHECK_INT(len, FRAMING_MAX_PAYLOAD);
    CHECK(payload != NULL && payload[0] == 'x' && payload[len - 1] == 'x');
    CHECK_INT(framing_decoder_buffered(d), 0);

    /* One byte more, terminated: a line this long cannot be a message of this
     * protocol, so it is refused instead of handed out. */
    framing_decoder_reset(d);
    line[FRAMING_MAX_PAYLOAD] = 'x'; /* undo the legal terminator above */
    line[FRAMING_MAX_PAYLOAD + 1] = FRAMING_TERMINATOR;
    CHECK_INT(framing_decoder_feed(d, line, FRAMING_MAX_FRAME + 1), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_ERR_LENGTH);

    /* Sticky until reset, and a later feed does not talk it out of the error:
     * the stream is no longer trustworthy, so the caller drops the link. */
    CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_ERR_LENGTH);
    CHECK_INT(framing_decoder_feed(d, "x", 1), FRAMING_ERR_LENGTH);
    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_buffered(d), 0);
    expect_need_more(d);

    /* Unterminated: at the cap the decoder must still wait -- the very next byte
     * may be the terminator that makes it a legal maximum-size line -- and one
     * byte past the cap it must refuse rather than keep growing the buffer. */
    line[FRAMING_MAX_PAYLOAD + 1] = 'x'; /* no terminator in the buffer at all */
    CHECK_INT(framing_decoder_feed(d, line, FRAMING_MAX_PAYLOAD), FRAMING_OK);
    expect_need_more(d);
    CHECK_INT(framing_decoder_feed(d, line, 1), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &payload, &len), FRAMING_ERR_LENGTH);
    framing_decoder_reset(d);

    /* What the net layer does with a refused line: reset and the next session
     * starts clean. */
    for (i = 0; i < 3; i++)
        CHECK_INT(framing_decoder_feed(d, "x", 1), FRAMING_OK);
    expect_need_more(d);

    free(line);
    framing_decoder_free(d);
}

static void test_one_megabyte_round_trip(void)
{
    framing_decoder *d = framing_decoder_new();
    size_t size = FRAMING_MAX_PAYLOAD;
    unsigned char *payload = malloc(size);
    unsigned char *frame = malloc(FRAMING_MAX_FRAME);
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

    CHECK_INT(framing_encoded_size(size), FRAMING_MAX_FRAME);
    CHECK_INT(framing_encode(payload, size, frame, FRAMING_MAX_FRAME, &written),
              FRAMING_OK);
    CHECK_INT(written, FRAMING_MAX_FRAME);
    CHECK(frame[0] == '{' && frame[size] == FRAMING_TERMINATOR);

    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    CHECK_INT(framing_decoder_next(d, &out, &out_len), FRAMING_OK);
    CHECK_INT(out_len, size);
    /* Guarded: a decoder that refuses this line leaves `out` NULL, and that has to
     * read as a failed check rather than as a crash that hides every later one. */
    CHECK(out != NULL && memcmp(out, payload, size) == 0);

    /* One byte over the limit cannot even be encoded. */
    CHECK_INT(framing_encode(payload, size + 1, frame, FRAMING_MAX_FRAME, &written),
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

    /* out_cap is the only length the encoder knows, so it must be the real one:
     * one byte short is short, and the terminator still has to fit. */
    CHECK_INT(framing_encode("hello", 5, out, 5, &written), FRAMING_ERR_SPACE);
    CHECK_INT(written, 123); /* untouched on failure */
    CHECK_INT(framing_encode("hello", 5, out, 6, &written), FRAMING_OK);
    CHECK_INT(written, 6);
    CHECK(memcmp(out, "hello", 5) == 0 && out[5] == FRAMING_TERMINATOR);
    CHECK_INT(framing_encode("hello", 5, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(framing_encode("hello", 5, NULL, 0, NULL), FRAMING_ERR_SPACE);
    /* An empty payload still needs its terminator. */
    CHECK_INT(framing_encode("", 0, out, 0, &written), FRAMING_ERR_SPACE);
    CHECK_INT(framing_encode("", 0, NULL, 1, &written), FRAMING_ERR_SPACE);
}

/* The one bug this encoder exists to refuse: a payload with a raw terminator
 * would leave this process as two messages, and the peer would believe both. */
static void test_encode_refuses_terminator_in_payload(void)
{
    unsigned char out[16];
    size_t written = 123;
    static const char bad[] = "{\"a\":1}\n{\"a\":2}";
    static const char trailing[] = "{\"a\":1}\n";

    memset(out, 0xAA, sizeof out);
    CHECK_INT(framing_encode(bad, sizeof bad - 1, out, sizeof out, &written),
              FRAMING_ERR_CONTENT);
    CHECK_INT(written, 123); /* untouched: nothing was written */
    CHECK(out[0] == 0xAA);

    CHECK_INT(framing_encode(trailing, sizeof trailing - 1, out, sizeof out, &written),
              FRAMING_ERR_CONTENT);
    CHECK(out[0] == 0xAA);

    /* And the check is exactly about the terminator: a CR inside a payload is
     * ordinary data on this wire, so CRLF never becomes a second framing rule. */
    CHECK_INT(framing_encode("a\rb", 3, out, sizeof out, &written), FRAMING_OK);
    CHECK_INT(written, 4);
    CHECK(out[1] == '\r' && out[3] == FRAMING_TERMINATOR);

    /* The decoded side agrees: a CR is delivered inside the payload. */
    {
        framing_decoder *d = framing_decoder_new();

        CHECK_INT(framing_decoder_feed(d, out, written), FRAMING_OK);
        expect_frame(d, "a\rb", 3);
        framing_decoder_free(d);
    }
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
    CHECK(out != NULL && memcmp(out, payload, sizeof payload) == 0);

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

/* The course document's rule for a bad message (§5.3) is "log it and drop it":
 * one unparsable line must cost that line and nothing else, and the decoder's
 * job is to hand the parser the next line as if the first had never been
 * written. */
static void test_bad_line_does_not_shift_the_next_one(void)
{
    framing_decoder *d = framing_decoder_new();
    static const char stream[] = "{\"broken\":\n{\"type\":\"heartbeat\"}\n";
    unsigned char frame[64];
    size_t written = 0;

    CHECK_INT(framing_decoder_feed(d, stream, sizeof stream - 1), FRAMING_OK);
    expect_frame(d, "{\"broken\":", 10); /* the parser rejects this one */
    expect_frame(d, "{\"type\":\"heartbeat\"}", 20);
    expect_need_more(d);

    /* The same when the two lines arrive as separate reads. */
    framing_decoder_reset(d);
    CHECK_INT(framing_decoder_feed(d, "not json\n", 9), FRAMING_OK);
    expect_frame(d, "not json", 8);
    CHECK_INT(framing_encode("{\"ok\":1}", 8, frame, sizeof frame, &written),
              FRAMING_OK);
    CHECK_INT(framing_decoder_feed(d, frame, written), FRAMING_OK);
    expect_frame(d, "{\"ok\":1}", 8);

    framing_decoder_free(d);
}

int main(void)
{
    printf("test_framing: one JSON message per line, '\\n' terminated\n");

    test_line_layout();
    test_single_frame();
    test_split_across_reads();
    test_two_frames_one_buffer();
    test_zero_length_line();
    test_line_length_limit();
    test_one_megabyte_round_trip();
    test_encode_rejects_small_buffers();
    test_encode_refuses_terminator_in_payload();
    test_binary_safe_payload();
    test_decoder_reuse();
    test_bad_line_does_not_shift_the_next_one();

    printf("test_framing: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
