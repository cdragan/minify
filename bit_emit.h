/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint8_t *begin;
    uint8_t *end;
    uint32_t data;
} BIT_EMITTER;

void init_bit_emitter(BIT_EMITTER *emitter, uint8_t *buf, size_t size);
void emit_bit(BIT_EMITTER *emitter, uint32_t bit);
void emit_bits(BIT_EMITTER *emitter, size_t value, int bits);
size_t emit_tail(BIT_EMITTER *emitter);
