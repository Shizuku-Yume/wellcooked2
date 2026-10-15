/*
 * framing.c -- one JSON message per line, as protocol/protocol.md §1 defines it.
 * See framing.h for the contract.
 */
#include <kitchenos/framing.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct framing_decoder {
    unsigned char *buf;
    size_t len; /* bytes held in buf */
    size_t pos; /* bytes already handed out */
    size_t cap;
    int error;  /* sticky: an over-long line means the stream is out of sync */
    int sys_errno; /* errno of the last failed allocation */
};

framing_decoder *framing_decoder_new(void)
{
    framing_decoder *d = calloc(1, sizeof *d);
    if (d != NULL)
        d->error = FRAMING_OK;
    return d;
}

void framing_decoder_free(framing_decoder *decoder)
{
    if (decoder == NULL)
        return;
    free(decoder->buf);
    free(decoder);
}

void framing_decoder_reset(framing_decoder *decoder)
{
    if (decoder == NULL)
        return;
    decoder->len = 0;
    decoder->pos = 0;
    decoder->error = FRAMING_OK;
    decoder->sys_errno = 0;
}

size_t framing_decoder_buffered(const framing_decoder *decoder)
{
    if (decoder == NULL)
        return 0;
    return decoder->len - decoder->pos;
}

int framing_decoder_errno(const framing_decoder *decoder)
{
    return decoder == NULL ? 0 : decoder->sys_errno;
}

/* Keeps the peak buffer at one line plus whatever a single feed() delivered, by
 * moving unconsumed bytes to the front before appending. */
static void compact(framing_decoder *d)
{
    if (d->pos == 0)
        return;
    if (d->len > d->pos)
        memmove(d->buf, d->buf + d->pos, d->len - d->pos);
    d->len -= d->pos;
    d->pos = 0;
}

int framing_decoder_feed(framing_decoder *decoder, const void *data, size_t len)
{
    size_t need;
    size_t cap;
    unsigned char *grown;

    if (decoder == NULL)
        return FRAMING_ERR_NOMEM;
    if (decoder->error != FRAMING_OK)
        return decoder->error;
    if (len == 0)
        return FRAMING_OK;
    if (data == NULL) {
        decoder->sys_errno = EINVAL;
        return FRAMING_ERR_NOMEM;
    }

    compact(decoder);

    if (len > (size_t)-1 - decoder->len) {
        decoder->sys_errno = EOVERFLOW;
        return FRAMING_ERR_NOMEM;
    }
    need = decoder->len + len;
    if (need > decoder->cap) {
        cap = decoder->cap ? decoder->cap : 256;
        while (cap < need) {
            if (cap > ((size_t)-1) / 2) {
                decoder->sys_errno = EOVERFLOW;
                return FRAMING_ERR_NOMEM;
            }
            cap *= 2;
        }
        errno = 0;
        grown = realloc(decoder->buf, cap);
        if (grown == NULL) {
            decoder->sys_errno = errno;
            return FRAMING_ERR_NOMEM;
        }
        decoder->buf = grown;
        decoder->cap = cap;
    }

    memcpy(decoder->buf + decoder->len, data, len);
    decoder->len += len;
    return FRAMING_OK;
}

int framing_decoder_next(framing_decoder *decoder, const char **payload, size_t *len)
{
    const unsigned char *start;
    const unsigned char *end;
    size_t avail;
    size_t frame_len;

    if (decoder == NULL)
        return FRAMING_ERR_NOMEM;
    if (decoder->error != FRAMING_OK)
        return decoder->error;

    avail = decoder->len - decoder->pos;
    if (avail == 0)
        return FRAMING_NEED_MORE;

    start = decoder->buf + decoder->pos;
    end = memchr(start, FRAMING_TERMINATOR, avail);
    if (end == NULL) {
        /* Nothing complete yet -- unless the line has already grown past the cap,
         * in which case waiting for its terminator is waiting for a message this
         * protocol cannot have. Refusing here also bounds what a peer that never
         * sends a terminator can make us buffer. */
        if (avail > (size_t)FRAMING_MAX_PAYLOAD) {
            decoder->error = FRAMING_ERR_LENGTH;
            return FRAMING_ERR_LENGTH;
        }
        return FRAMING_NEED_MORE;
    }

    frame_len = (size_t)(end - start);
    if (frame_len > (size_t)FRAMING_MAX_PAYLOAD) {
        /* Terminated, but longer than any legal line: the same failure as above,
         * reported instead of handed to a caller that has no room for it. */
        decoder->error = FRAMING_ERR_LENGTH;
        return FRAMING_ERR_LENGTH;
    }

    if (payload != NULL)
        *payload = (const char *)start;
    if (len != NULL)
        *len = frame_len;

    decoder->pos += frame_len + FRAMING_TERMINATOR_LEN;
    if (decoder->pos == decoder->len) {
        decoder->pos = 0;
        decoder->len = 0;
    }
    return FRAMING_OK;
}

size_t framing_encoded_size(size_t payload_len)
{
    if (payload_len > (size_t)FRAMING_MAX_PAYLOAD)
        return 0; /* framing_encode() reports FRAMING_ERR_LENGTH for this size */
    return payload_len + FRAMING_TERMINATOR_LEN;
}

int framing_encode(const void *payload, size_t payload_len,
                   void *out, size_t out_cap, size_t *written)
{
    unsigned char *bytes = out;
    size_t total;

    if (payload_len > (size_t)FRAMING_MAX_PAYLOAD)
        return FRAMING_ERR_LENGTH;
    if (out == NULL || payload_len > (size_t)-1 - FRAMING_TERMINATOR_LEN)
        return FRAMING_ERR_SPACE;
    total = payload_len + FRAMING_TERMINATOR_LEN;
    if (out_cap < total)
        return FRAMING_ERR_SPACE;
    if (payload_len > 0 && payload == NULL)
        return FRAMING_ERR_SPACE;

    /* A terminator inside the payload would end this message early, and the rest
     * of the payload would reach the peer as a second, truncated message. JSON
     * escapes LF as \n, so this is a caller bug, not a message; the encoder
     * refuses to be the thing that puts it on the wire. */
    if (payload_len > 0 && memchr(payload, FRAMING_TERMINATOR, payload_len) != NULL)
        return FRAMING_ERR_CONTENT;

    if (payload_len > 0)
        memcpy(bytes, payload, payload_len);
    bytes[payload_len] = (unsigned char)FRAMING_TERMINATOR;
    if (written != NULL)
        *written = total;
    return FRAMING_OK;
}
