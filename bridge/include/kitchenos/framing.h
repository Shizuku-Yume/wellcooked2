/*
 * framing.h -- the wire framing shared with the game plugin.
 *
 * One message per line: the UTF-8 JSON bytes of exactly one message, then '\n'.
 * Nothing here interprets the payload -- semantic errors belong to json and
 * protocol -- and the terminator is the only boundary this layer looks for. That
 * is what keeps a bad message cheap: a reader that cannot parse a payload is
 * already at the start of the next message, so it logs and drops that one
 * message and the link lives on (course document §5.3). A length-prefixed stream
 * has no such answer -- a wrong length is indistinguishable from a wrong
 * boundary, and the only safe reaction is to drop the connection.
 *
 * The contract is protocol/protocol.md §1, implemented three times: here, in
 * plugin/WellCooked.Plugin/Net/TcpLink.cs and in tools/fake_plugin.py. The first
 * two were written in the same round, so "both ends agree" is agreement by
 * construction; the independent checks are plugin/tools/conformance/ (a peer
 * compiled from the plugin's own sources) and the real-game run. TcpLink.cs is
 * an implementation, not the truth: when it and this file disagree, the document
 * is what settles it.
 *
 * The decoder is a stream buffer, not a one-shot parser: TCP hands over arbitrary
 * chunks, so a line may arrive split across reads and several lines may arrive
 * in one read. Feed whatever arrived, then pull lines until it says "need more".
 */
#ifndef KITCHENOS_FRAMING_H
#define KITCHENOS_FRAMING_H

#include <stddef.h>

/* The whole of the framing overhead: one byte, and the one byte a payload may
 * not contain. LF, not CRLF -- a CR inside a payload is ordinary data, so this
 * layer neither writes nor strips one. */
#define FRAMING_TERMINATOR '\n'
#define FRAMING_TERMINATOR_LEN 1u

/* Longest payload a single line may carry, terminator excluded: the same number
 * as TcpLink.MaxFrameBytes and fake_plugin.MAX_FRAME_BYTES. A line that grows
 * past this without ending means the stream is not what we think it is (or the
 * peer is hostile), and nothing here can tell the next terminator from a byte
 * inside a payload: the caller must drop the connection rather than guess. */
#define FRAMING_MAX_PAYLOAD 1048576u

/* Longest legal line on the wire, terminator included -- the size of the buffer
 * a reader needs to hold one whole frame (see KOS_NET_DECODER_LIMIT). */
#define FRAMING_MAX_FRAME (FRAMING_MAX_PAYLOAD + FRAMING_TERMINATOR_LEN)

enum {
    FRAMING_OK = 0,
    FRAMING_NEED_MORE = 1,    /* decoder_next(): no complete line buffered yet */
    FRAMING_ERR_LENGTH = -1,  /* over FRAMING_MAX_PAYLOAD bytes with no line end */
    FRAMING_ERR_NOMEM = -2,   /* allocation failed */
    FRAMING_ERR_SPACE = -3,   /* encoder output buffer too small */
    FRAMING_ERR_CONTENT = -4  /* encoder: the payload contains the terminator */
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

/* Appends received bytes. Returns FRAMING_OK, or FRAMING_ERR_NOMEM. An over-long
 * line is not reported here: it is reported by the next framing_decoder_next()
 * that reaches it. */
int framing_decoder_feed(framing_decoder *decoder, const void *data, size_t len);

/* Hands out the next complete line, terminator excluded.
 *   FRAMING_OK         -> payload and len describe one message, valid until the next
 *                         feed()/reset()/free() of the same decoder. A zero-length
 *                         line is a legal frame here and yields len == 0; it is the
 *                         caller's parser that rejects the empty message while the
 *                         link stays up.
 *   FRAMING_NEED_MORE  -> no terminator buffered yet; feed again.
 *   FRAMING_ERR_LENGTH -> a line that cannot be legal: either the buffer already
 *                         holds more than FRAMING_MAX_PAYLOAD bytes with no
 *                         terminator, or the line that ends in one is longer than
 *                         that. Sticky until reset, and the caller must not try to
 *                         resynchronize the stream. */
int framing_decoder_next(framing_decoder *decoder, const char **payload, size_t *len);

/* Number of bytes framing_encode() writes for a payload of that size: the payload
 * plus its terminator. 0 for a size framing_encode() refuses with
 * FRAMING_ERR_LENGTH. */
size_t framing_encoded_size(size_t payload_len);

/* Writes payload + '\n' into `out`. Fails with FRAMING_ERR_LENGTH when the
 * payload would exceed FRAMING_MAX_PAYLOAD, FRAMING_ERR_CONTENT when the payload
 * contains the terminator byte, FRAMING_ERR_SPACE when out_cap is smaller than
 * framing_encoded_size(payload_len). `written` is optional and left untouched on
 * failure.
 *
 * FRAMING_ERR_CONTENT exists because a terminator inside a payload is not a
 * message that fails to parse -- it is two messages, one of them truncated, and
 * the peer would believe both. JSON on this wire escapes LF as \n, so only a bug
 * can produce one; the encoder refuses it so that the bug cannot leave this
 * process. */
int framing_encode(const void *payload, size_t payload_len,
                   void *out, size_t out_cap, size_t *written);

#endif /* KITCHENOS_FRAMING_H */
