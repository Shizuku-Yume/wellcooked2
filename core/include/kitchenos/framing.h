/*
 * framing.h -- the wire framing shared with the game plugin.
 *
 * Truth is plugin/WellCooked.Plugin/Net/TcpLink.cs: a 4-byte BIG-ENDIAN unsigned
 * length prefix followed by that many bytes of UTF-8 JSON, one frame capped at
 * 1 MiB. Nothing here interprets the payload -- semantic errors belong to json
 * and protocol.
 *
 * The decoder is a stream buffer, not a one-shot parser: TCP hands over arbitrary
 * chunks, so a frame may arrive split across reads and several frames may arrive
 * in one read. Feed whatever arrived, then pull frames until it says "need more".
 */
#ifndef KITCHENOS_FRAMING_H
#define KITCHENOS_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#define FRAMING_HEADER_LEN 4

/* Same value as TcpLink.MaxFrameBytes. A length outside [0, this] means the
 * stream lost sync (or the peer is hostile): the caller must drop the connection
 * rather than guess where the next frame starts. */
#define FRAMING_MAX_PAYLOAD 1048576u

enum {
    FRAMING_OK = 0,
    FRAMING_NEED_MORE = 1,    /* decoder_next(): no complete frame buffered yet */
    FRAMING_ERR_LENGTH = -1,  /* frame length negative or above FRAMING_MAX_PAYLOAD */
    FRAMING_ERR_NOMEM = -2,   /* allocation failed */
    FRAMING_ERR_SPACE = -3    /* encoder output buffer too small */
};

typedef struct framing_decoder framing_decoder; /* opaque */

framing_decoder *framing_decoder_new(void);
void framing_decoder_free(framing_decoder *decoder);

/* Drops buffered input and clears a sticky length error, so the decoder can be
 * reused after a reconnect. */
void framing_decoder_reset(framing_decoder *decoder);

/* Number of bytes buffered but not yet handed out. */
size_t framing_decoder_buffered(const framing_decoder *decoder);

/* errno of the last failed allocation in this decoder, 0 if none: the caller
 * needs it to write a log line that says what actually went wrong. */
int framing_decoder_errno(const framing_decoder *decoder);

/* Appends received bytes. Returns FRAMING_OK, or FRAMING_ERR_NOMEM. A bad length
 * prefix is not reported here: it is reported by the next framing_decoder_next()
 * that reaches it. */
int framing_decoder_feed(framing_decoder *decoder, const void *data, size_t len);

/* Hands out the next complete frame.
 *   FRAMING_OK         -> payload and len describe one frame, valid until the next
 *                         feed()/reset()/free() of the same decoder. A zero-length
 *                         frame is legal here and yields len == 0.
 *   FRAMING_NEED_MORE  -> incomplete frame; feed again.
 *   FRAMING_ERR_LENGTH -> length prefix out of range; sticky until reset. The
 *                         caller must not try to resynchronize the stream. */
int framing_decoder_next(framing_decoder *decoder, const char **payload, size_t *len);

/* Number of bytes framing_encode() writes for a payload of that size. */
size_t framing_encoded_size(size_t payload_len);

/* Writes header + payload into `out`. Fails with FRAMING_ERR_LENGTH when the
 * payload would exceed FRAMING_MAX_PAYLOAD, FRAMING_ERR_SPACE when out_cap is
 * smaller than framing_encoded_size(payload_len). `written` is optional. */
int framing_encode(const void *payload, size_t payload_len,
                   void *out, size_t out_cap, size_t *written);

/* Reads a length prefix without consuming anything: lets a reader tell "the peer
 * announced an impossible frame" apart from "the peer is still sending". */
int framing_peek_length(const unsigned char header[FRAMING_HEADER_LEN],
                        size_t *payload_len);

#endif /* KITCHENOS_FRAMING_H */
