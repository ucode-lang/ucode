/*
 * Copyright (C) 2020-2021 Jo-Philipp Wich <jo@mein.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>

#include "ucode/internal/chunk.h"

#define OFFSETINFO_MAX_BYTES 127
#define OFFSETINFO_MAX_INSNS 127
#define OFFSETINFO_NUM_BYTES(o) ((o)->bytes & OFFSETINFO_MAX_BYTES)
#define OFFSETINFO_NUM_INSNS(o) ((o)->insns & OFFSETINFO_MAX_INSNS)
#define OFFSETINFO_IS_END(o) ((o)->insns & 0x80)


void
uc_chunk_init(uc_chunk_t *chunk)
{
	chunk->count = 0;
	chunk->entries = NULL;

	chunk->ehranges.count = 0;
	chunk->ehranges.entries = NULL;

	chunk->debuginfo.offsets.count = 0;
	chunk->debuginfo.offsets.entries = NULL;

	chunk->debuginfo.variables.count = 0;
	chunk->debuginfo.variables.entries = NULL;

	uc_vallist_init(&chunk->debuginfo.varnames);
}

void
uc_chunk_free(uc_chunk_t *chunk)
{
	uc_vector_clear(chunk);
	uc_vector_clear(&chunk->ehranges);

	uc_vector_clear(&chunk->debuginfo.offsets);
	uc_vector_clear(&chunk->debuginfo.variables);
	uc_vallist_free(&chunk->debuginfo.varnames);

	uc_chunk_init(chunk);
}

size_t
uc_chunk_add(uc_chunk_t *chunk, uint8_t byte, size_t offset)
{
	uc_offsetinfo_t *offsets = &chunk->debuginfo.offsets;
	size_t i;

	uc_vector_push(chunk, byte);

	/* Offset info is encoded in byte pairs, the first byte specifies the number
	 * of source text bytes to advance since the last entry and the second byte
	 * specifies the amount of instructions belonging to the source text offset.
	 * Byte and instruction count values are limited to 7 bits (0x00..0x7f),
	 * the most significant bit in each byte is reserved as flag value; if the
	 * bit is set in the first byte, it signals the begin of a logical statement
	 * while a set bit in the second byte denotes the end of the statement. */
	if (offset > 0 || offsets->count == 0) {
		/* If this offset is farther than 127 (2 ** 7 - 1) bytes apart from
		 * the last one, we need to emit intermediate "jump" bytes with zero
		 * instructions each */
		for (i = offset; i > OFFSETINFO_MAX_BYTES; i -= OFFSETINFO_MAX_BYTES) {
			/* advance by 127 bytes */
			uc_vector_push(offsets, { OFFSETINFO_MAX_BYTES, 0 });
		}

		/* advance by `i` bytes, count one instruction */
		uc_vector_push(offsets, { i, 1 });
	}

	/* update instruction count at current offset entry */
	else {
		uc_offset_t *o = uc_vector_last(offsets);

		/* since we encode the per-offset instruction count in seven bits, we
		 * can only count up to 127 instructions. If we exceed that limit,
		 * emit another offset entry with the byte offset set to zero */
		if (OFFSETINFO_NUM_INSNS(o) >= OFFSETINFO_MAX_INSNS) {
			/* advance by 0 bytes, count one instruction */
			uc_vector_push(offsets, { 0, 1 });
		}
		else {
			o->insns++;
		}
	}

	return chunk->count - 1;
}

void
uc_chunk_stmt_start(uc_chunk_t *chunk, size_t offset)
{
	uc_offsetinfo_t *offsets = &chunk->debuginfo.offsets;
	size_t i;

	for (i = offset; i > OFFSETINFO_MAX_BYTES; i -= OFFSETINFO_MAX_BYTES) {
		/* advance by 127 bytes */
		uc_vector_push(offsets, { OFFSETINFO_MAX_BYTES, 0 });
	}

	/* advance by `i` bytes, set start of statement flag */
	uc_vector_push(offsets, { i | 0x80, 0 });
}

void
uc_chunk_stmt_end(uc_chunk_t *chunk, size_t offset)
{
	uc_offsetinfo_t *offsets = &chunk->debuginfo.offsets;
	uc_offset_t *o = offsets->count ? uc_vector_last(offsets) : NULL;
	size_t i;

	for (i = offset; i > OFFSETINFO_MAX_BYTES; i -= OFFSETINFO_MAX_BYTES) {
		/* advance by 127 bytes */
		uc_vector_push(offsets, { OFFSETINFO_MAX_BYTES, 0 });
	}

	if (i > 0 || o == NULL || OFFSETINFO_IS_END(o)) {
		/* advance by `i` bytes, set start of statement flag */
		uc_vector_push(offsets, { i, 0x80 });
	}
	else {
		/* set end flag on last offset entry */
		o->insns |= 0x80;
	}
}

void
uc_chunk_pop(uc_chunk_t *chunk)
{
	assert(chunk->count > 0);

	chunk->count--;

	for (size_t i = chunk->debuginfo.offsets.count; i > 0; i--) {
		uc_offset_t *o = &chunk->debuginfo.offsets.entries[i - 1];

		if (o->insns & 127) {
			o->insns = ((o->insns & 127) - 1) | (o->insns & 128);
			break;
		}
	}
}

/* Resolve the innermost statement enclosing the instruction at `off`, storing
 * the source offsets delimiting its extent into `startoff` and `endoff`.
 * Returns false, leaving both untouched, if no statement covers that
 * instruction - e.g. because it belongs to implicitly emitted code such as a
 * function's trailing `return null`. A caller which only wants the best
 * available bounds may therefore initialize them to an empty range and ignore
 * the return value.
 *
 * An `off` beyond the last instruction - SIZE_MAX, say - resolves the innermost
 * statement which was started but never closed, i.e. the one compilation was
 * inside of when a syntax error aborted it. Since the error kept the end marker
 * from being emitted, `endoff` then denotes the end of the emitted prefix
 * rather than of the statement in the source text.
 *
 * Statement spans nest, but an inner one always closes before its enclosing
 * one, so the first closed span found to cover `off` is the innermost match.
 * Having passed a non-matching span, the next candidate can only ever be the
 * span compiled around it, so remembering a single parent suffices: giving up
 * more than one level out makes the walk come up empty, but never return a
 * span which doesn't actually enclose `off`. The few instructions which fall
 * through this - loop back-edges, function return tails and the like, none of
 * which can be a failing expression - are left for the caller to render as a
 * plain position marker. */
bool
uc_chunk_debug_stmt_bounds(uc_chunk_t *chunk, size_t off, size_t *startoff, size_t *endoff)
{
	uc_offsetinfo_t *offsets = &chunk->debuginfo.offsets;
	struct { size_t pos, insn; } stack[2];
	int depth = -1;
	size_t i, bnum = 0, inum = 0, ins;

	for (i = 0; i < offsets->count; i++) {
		uc_offset_t *o = &offsets->entries[i];

		/* the byte delta of an entry leads up to the position it describes,
		 * while its instruction count covers instructions emitted afterwards,
		 * since writing instructions bumps the last entry's counter; hence
		 * `inum` is captured before, `bnum` after accumulating */
		ins = inum;

		bnum += OFFSETINFO_NUM_BYTES(o);
		inum += OFFSETINFO_NUM_INSNS(o);

		/* a set high bit in the byte count opens a statement */
		if (o->bytes & 0x80) {
			if (depth == 1)
				stack[0] = stack[1];
			else
				depth++;

			stack[depth].pos = bnum;
			stack[depth].insn = ins;
		}

		/* a set high bit in the instruction count closes it again */
		if (!(o->insns & 0x80) || depth < 0)
			continue; /* unmatched end flag, ignore */

		if (off >= stack[depth].insn && off < inum) {
			*startoff = stack[depth].pos;
			*endoff = bnum;

			return true;
		}

		depth--;
	}

	/* No closed statement covered `off`. If one is still open at that point -
	 * compilation aborted midway through it, leaving the end flag unemitted -
	 * then its emitted prefix is the best match available. Since statements
	 * are only ever pushed, `open` is the innermost one here. */
	if (depth >= 0 && off >= stack[depth].insn) {
		*startoff = stack[depth].pos;
		*endoff = bnum;

		return true;
	}

	return false;
}

size_t
uc_chunk_debug_get_srcpos(uc_chunk_t *chunk, size_t off)
{
	uc_offsetinfo_t *offsets = &chunk->debuginfo.offsets;
	size_t i, inum = 0, bnum = 0;

	if (!offsets->count)
		return 0;

	for (i = 0; i < offsets->count && inum < off; i++) {
		bnum += OFFSETINFO_NUM_BYTES(&offsets->entries[i]);
		inum += OFFSETINFO_NUM_INSNS(&offsets->entries[i]);
	}

	return bnum;
}

void
uc_chunk_debug_add_variable(uc_chunk_t *chunk, size_t from, size_t to, size_t slot, bool upval, uc_value_t *name)
{
	uc_variables_t *variables = &chunk->debuginfo.variables;
	uc_value_list_t *varnames = &chunk->debuginfo.varnames;

	assert(slot <= ((size_t)-1 / 2));

	if (upval)
		slot += (size_t)-1 / 2;

	uc_vector_push(variables, {
		.nameidx = uc_vallist_add(varnames, name),
		.slot    = slot,
		.from    = from,
		.to      = to
	});
}

uc_value_t *
uc_chunk_debug_get_variable(uc_chunk_t *chunk, size_t off, size_t slot, bool upval)
{
	uc_variables_t *variables = &chunk->debuginfo.variables;
	uc_value_list_t *varnames = &chunk->debuginfo.varnames;
	uc_value_t *name = NULL;
	size_t i;

	assert(slot <= ((size_t)-1 / 2));

	if (upval)
		slot += (size_t)-1 / 2;

	for (i = 0; i < variables->count; i++) {
		if (variables->entries[i].slot != slot ||
		    variables->entries[i].from > off ||
		    variables->entries[i].to < off)
			continue;

		name = uc_vallist_get(varnames, variables->entries[i].nameidx);
	}

	return name;
}
