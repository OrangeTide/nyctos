/* wire.h : a bounds-checked cursor over a message payload.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Append and consume fixed-width fields and byte runs over the inline
 * message payload, little-endian, without casting a struct onto bytes a
 * client controls. A writer refuses to run past its buffer and a reader
 * refuses to read past the bytes it was given; each carries a sticky ok
 * flag that stays clear once an overrun is seen. A caller writes or reads
 * a whole message and then checks the flag once. The filesystem and block
 * servers and their clients use it to encode requests and replies.
 *
 */

#ifndef _SYS_WIRE_H_
#define _SYS_WIRE_H_

#include <stdint.h>

/*
 * Append cursor. Fields go out little-endian. A put that would pass the
 * capacity clears ok and writes nothing, so the first overrun is sticky and
 * every later put is a no-op until the caller checks ok.
 */
struct wire_out {
    uint8_t     *buf;
    uint32_t    cap;
    uint32_t    len;
    int         ok;
};

static inline void
wire_out_init(struct wire_out *o, uint8_t *buf, uint32_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->ok = 1;
}

static inline void
wire_put_u8(struct wire_out *o, uint8_t v)
{
    if (!o->ok || o->len >= o->cap) {
        o->ok = 0;
        return;
    }
    o->buf[o->len++] = v;
}

static inline void
wire_put_u16(struct wire_out *o, uint16_t v)
{
    wire_put_u8(o, (uint8_t)v);
    wire_put_u8(o, (uint8_t)(v >> 8));
}

static inline void
wire_put_u32(struct wire_out *o, uint32_t v)
{
    wire_put_u16(o, (uint16_t)v);
    wire_put_u16(o, (uint16_t)(v >> 16));
}

static inline void
wire_put_u64(struct wire_out *o, uint64_t v)
{
    wire_put_u32(o, (uint32_t)v);
    wire_put_u32(o, (uint32_t)(v >> 32));
}

/*
 * The remaining-count form, cap - len, cannot overflow because len is never
 * advanced past cap, so an attacker-sized n is compared safely.
 */
static inline void
wire_put_bytes(struct wire_out *o, const uint8_t *src, uint32_t n)
{
    uint32_t i;

    if (!o->ok || n > o->cap - o->len) {
        o->ok = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        o->buf[o->len++] = src[i];
    }
}

/* Zero-fill n bytes, for the padding between a name and the next entry. */
static inline void
wire_put_zeros(struct wire_out *o, uint32_t n)
{
    uint32_t i;

    if (!o->ok || n > o->cap - o->len) {
        o->ok = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        o->buf[o->len++] = 0;
    }
}

/*
 * Consume cursor. Mirrors the append cursor. A get past the end clears ok and
 * yields zero, so a length a client declares cannot drag the position past
 * the bytes the caller owns.
 */
struct wire_in {
    const uint8_t *buf;
    uint32_t    len;
    uint32_t    pos;
    int         ok;
};

static inline void
wire_in_init(struct wire_in *in, const uint8_t *buf, uint32_t len)
{
    in->buf = buf;
    in->len = len;
    in->pos = 0;
    in->ok = 1;
}

static inline uint8_t
wire_get_u8(struct wire_in *in)
{
    if (!in->ok || in->pos >= in->len) {
        in->ok = 0;
        return 0;
    }
    return in->buf[in->pos++];
}

static inline uint16_t
wire_get_u16(struct wire_in *in)
{
    uint16_t a = wire_get_u8(in);
    uint16_t b = wire_get_u8(in);
    return (uint16_t)(a | (uint16_t)(b << 8));
}

static inline uint32_t
wire_get_u32(struct wire_in *in)
{
    uint32_t a = wire_get_u16(in);
    uint32_t b = wire_get_u16(in);
    return a | (b << 16);
}

static inline uint64_t
wire_get_u64(struct wire_in *in)
{
    uint64_t a = wire_get_u32(in);
    uint64_t b = wire_get_u32(in);
    return a | (b << 32);
}

/* Copy n bytes into dst. On underflow, clears ok and copies nothing. */
static inline void
wire_get_bytes(struct wire_in *in, uint8_t *dst, uint32_t n)
{
    uint32_t i;

    if (!in->ok || n > in->len - in->pos) {
        in->ok = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        dst[i] = in->buf[in->pos++];
    }
}

/* Advance over n bytes without copying, for padding and skipped fields. */
static inline void
wire_skip(struct wire_in *in, uint32_t n)
{
    if (!in->ok || n > in->len - in->pos) {
        in->ok = 0;
        return;
    }
    in->pos += n;
}

#endif /* _SYS_WIRE_H_ */
