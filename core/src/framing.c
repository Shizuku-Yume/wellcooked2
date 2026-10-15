/*
 * framing.c -- 4-byte big-endian length prefix + UTF-8 payload, as TcpLink.cs
 * writes and reads it. See framing.h for the contract.
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
    int error;  /* sticky: a bad length prefix means the stream is out of sync */
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

/* Keeps the peak buffer at one frame plus whatever a single feed() delivered,
 * by moving unconsumed bytes to the front before appending. */
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

int framing_peek_length(const unsigned char header[FRAMING_HEADER_LEN],
                        size_t *payload_len)
{
    uint32_t raw;

    if (header == NULL)
        return FRAMING_ERR_LENGTH;
    raw = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
          ((uint32_t)header[2] << 8) | (uint32_t)header[3];
    /* TcpLink reads the prefix into a signed int, so "negative length" and
     * "length above 1 MiB" are the same rejection on both ends; 0xFFFFFFFF lands
     * here as 4294967295 and is refused. */
    if (raw > FRAMING_MAX_PAYLOAD)
        return FRAMING_ERR_LENGTH;
    if (payload_len != NULL)
        *payload_len = (size_t)raw;
    return FRAMING_OK;
}

int framing_decoder_next(framing_decoder *decoder, const char **payload, size_t *len)
{
    size_t avail;
    size_t frame_len = 0;
    int rc;

    if (decoder == NULL)
        return FRAMING_ERR_NOMEM;
    if (decoder->error != FRAMING_OK)
        return decoder->error;

    avail = decoder->len - decoder->pos;
    if (avail < FRAMING_HEADER_LEN)
        return FRAMING_NEED_MORE;

    rc = framing_peek_length(decoder->buf + decoder->pos, &frame_len);
    if (rc != FRAMING_OK) {
        decoder->error = rc;
        return rc;
    }
    if (avail - FRAMING_HEADER_LEN < frame_len)
        return FRAMING_NEED_MORE;

    if (payload != NULL)
        *payload = (const char *)(decoder->buf + decoder->pos + FRAMING_HEADER_LEN);
    if (len != NULL)
        *len = frame_len;

    decoder->pos += FRAMING_HEADER_LEN + frame_len;
    if (decoder->pos == decoder->len) {
        decoder->pos = 0;
        decoder->len = 0;
    }
    return FRAMING_OK;
}

size_t framing_encoded_size(size_t payload_len)
{
    if (payload_len > FRAMING_MAX_PAYLOAD)
        return 0; /* framing_encode() reports FRAMING_ERR_LENGTH for this size */
    return payload_len + FRAMING_HEADER_LEN;
}

int framing_encode(const void *payload, size_t payload_len,
                   void *out, size_t out_cap, size_t *written)
{
    unsigned char *bytes = out;
    size_t total;

    if (payload_len > FRAMING_MAX_PAYLOAD)
        return FRAMING_ERR_LENGTH;
    if (out == NULL || payload_len > (size_t)-1 - FRAMING_HEADER_LEN)
        return FRAMING_ERR_SPACE;
    total = payload_len + FRAMING_HEADER_LEN;
    if (out_cap < total)
        return FRAMING_ERR_SPACE;
    if (payload_len > 0 && payload == NULL)
        return FRAMING_ERR_SPACE;

    bytes[0] = (unsigned char)((payload_len >> 24) & 0xFFu);
    bytes[1] = (unsigned char)((payload_len >> 16) & 0xFFu);
    bytes[2] = (unsigned char)((payload_len >> 8) & 0xFFu);
    bytes[3] = (unsigned char)(payload_len & 0xFFu);
    if (payload_len > 0)
        memcpy(bytes + FRAMING_HEADER_LEN, payload, payload_len);
    if (written != NULL)
        *written = total;
    return FRAMING_OK;
}
