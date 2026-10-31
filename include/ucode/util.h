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

/* Failsafe x* allocators and ALIGN / ARRAY_SIZE helpers. */

#ifndef UCODE_UTIL_H
#define UCODE_UTIL_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h> /* va_start(), va_end(), va_list */
#include <string.h> /* strdup() */


/* alignment & array size */

#ifndef ALIGN
#define ALIGN(x) (((x) + sizeof(size_t) - 1) & -sizeof(size_t))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif


/* "failsafe" utility functions */

static inline void *xcalloc(size_t size, size_t nmemb) {
	void *ptr = calloc(size, nmemb);

	if (!ptr) {
		fprintf(stderr, "Out of memory\n");
		abort();
	}

	return ptr;
}

static inline void *xalloc(size_t size) {
	return xcalloc(1, size);
}

static inline void *xrealloc(void *ptr, size_t size) {
	ptr = realloc(ptr, size);

	if (!ptr) {
		fprintf(stderr, "Out of memory\n");
		abort();
	}

	return ptr;
}

static inline char *xstrdup(const char *s) {
	char *ptr = strdup(s);

	if (!ptr) {
		fprintf(stderr, "Out of memory\n");
		abort();
	}

	return ptr;
}

__attribute__((format(printf, 2, 0)))
static inline int xasprintf(char **strp, const char *fmt, ...) {
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vasprintf(strp, fmt, ap);
	va_end(ap);

	if (len == -1) {
		fprintf(stderr, "Out of memory\n");
		abort();
	}

	return len;
}

__attribute__((format(printf, 2, 0)))
static inline int xvasprintf(char **strp, const char *fmt, va_list ap) {
	int len = vasprintf(strp, fmt, ap);

	if (len == -1) {
		fprintf(stderr, "Out of memory\n");
		abort();
	}

	return len;
}

/* string buffers; `buf` is NUL-terminated after `bpos` bytes */

typedef struct {
	char *buf;
	int bpos;
	int size;
} uc_stringbuf_t;

uc_stringbuf_t *uc_stringbuf_new(void);
void uc_stringbuf_free(uc_stringbuf_t *sb);
void uc_stringbuf_grow(uc_stringbuf_t *sb, size_t size);
int uc_stringbuf_memset(uc_stringbuf_t *sb, int offset, int c, size_t len);
void uc_stringbuf_reset(uc_stringbuf_t *sb);
int uc_stringbuf_printf(uc_stringbuf_t *sb, const char *fmt, ...);

static inline void
uc_stringbuf_add(uc_stringbuf_t *sb, const char *s, size_t len)
{
	if ((size_t)sb->bpos + len + 1 > (size_t)sb->size)
		uc_stringbuf_grow(sb, (size_t)sb->bpos + len + 1);

	memcpy(sb->buf + sb->bpos, s, len);
	sb->bpos += len;
	sb->buf[sb->bpos] = 0;
}

#define uc_stringbuf_length(sb) ((sb)->bpos)
#define uc_stringbuf_append(sb, str) uc_stringbuf_add((sb), "" str, sizeof(str) - 1)


/* linked lists */

typedef struct uc_list {
	struct uc_list *prev;
	struct uc_list *next;
} uc_list_t;

static inline void uc_list_insert(uc_list_t *list, uc_list_t *item)
{
	list->next->prev = item;
	item->next = list->next;
	item->prev = list;
	list->next = item;
}

static inline void uc_list_remove(uc_list_t *item)
{
	item->next->prev = item->prev;
	item->prev->next = item->next;
	item->prev = item->next = item;
}

#define uc_list_foreach(item, list) \
	for (uc_list_t *item = (list)->next; item != (list); item = item->next)


/* sets of pointers, kept in insertion order in `entries`; items are equal
 * by `equal`, or as pointers if it is NULL, and `hash` must agree with it */

typedef struct {
	size_t count;
	void **entries;
	size_t *index;
	size_t mask;
	unsigned long (*hash)(const void *);
	int (*equal)(const void *, const void *);
} uc_set_t;

static inline unsigned long
uc_set_hash(uc_set_t *set, const void *item)
{
	return set->hash ? set->hash(item) : (unsigned long)((uintptr_t)item * 2654435761u);
}

static inline bool
uc_set_has(uc_set_t *set, const void *item)
{
	if (!set->index)
		return false;

	for (size_t i = uc_set_hash(set, item) & set->mask; set->index[i]; i = (i + 1) & set->mask) {
		void *e = set->entries[set->index[i] - 1];

		if (set->equal ? set->equal(e, item) : (e == item))
			return true;
	}

	return false;
}

static inline void
uc_set_index_add(uc_set_t *set, size_t pos)
{
	size_t i = uc_set_hash(set, set->entries[pos]) & set->mask;

	while (set->index[i])
		i = (i + 1) & set->mask;

	set->index[i] = pos + 1;
}

/* Adds `item` unless the set has it; returns whether it was added */
static inline bool
uc_set_add(uc_set_t *set, void *item)
{
	size_t slots = set->index ? set->mask + 1 : 0;

	if (uc_set_has(set, item))
		return false;

	if ((set->count + 1) * 4 > slots * 3) {
		slots = slots ? slots * 2 : 16;
		free(set->index);
		set->index = xcalloc(slots, sizeof(*set->index));
		set->mask = slots - 1;
		set->entries = xrealloc(set->entries, slots * sizeof(*set->entries));

		for (size_t pos = 0; pos < set->count; pos++)
			uc_set_index_add(set, pos);
	}

	set->entries[set->count] = item;
	uc_set_index_add(set, set->count++);

	return true;
}

static inline void
uc_set_free(uc_set_t *set)
{
	free(set->entries);
	free(set->index);
	set->entries = NULL;
	set->index = NULL;
	set->count = 0;
}


/* vector macros */

#define UC_VECTOR_INIT_SIZE 8

/* `capacity` is the number of entries allocated. Code that sets `entries`
 * itself leaves it as it is: NULL entries are allocated anew, and a capacity
 * of 0 next to allocated entries causes a reallocation at the next extend. */
#define uc_declare_vector(name, type) \
	typedef struct { \
		size_t count; \
		type *entries; \
		size_t capacity; \
	} name

static inline size_t
uc_vector_capacity(size_t init, size_t count)
{
	if (count == 0)
		return init;

	size_t capacity = init;

	while (capacity <= count)
		capacity += (capacity >> 1);

	return capacity;
}

static inline void
uc_vector_reduce_init_(char **base, size_t *capacity, size_t itemsize, size_t init,
                       size_t count, size_t remove)
{
	if (*base == NULL)
		return;

	if (remove > count)
		remove = count;

	size_t next_capacity = uc_vector_capacity(init, count - remove);

	if (*capacity && next_capacity >= *capacity)
		return;

	*base = (__typeof__(*base))xrealloc(*base, itemsize * next_capacity);
	*capacity = next_capacity;
}

static inline void *
uc_vector_extend_init_(char **base, size_t *capacity, size_t itemsize, size_t init,
                       size_t count, size_t add)
{
	if (*base == NULL || count + add >= *capacity) {
		size_t next_capacity = uc_vector_capacity((*base && *capacity) ? *capacity : init,
		                                          count + add);

		*base = (__typeof__(*base))xrealloc(*base, itemsize * next_capacity);
		*capacity = next_capacity;

		memset(*base + itemsize * count, 0,
			itemsize * (next_capacity - count));
	}

	return *base + itemsize * count;
}

static inline void
uc_vector_reduce_(char **base, size_t *capacity, size_t itemsize, size_t count, size_t remove)
{
	uc_vector_reduce_init_(base, capacity, itemsize, UC_VECTOR_INIT_SIZE, count, remove);
}

static inline void *
uc_vector_extend_(char **base, size_t *capacity, size_t itemsize, size_t count, size_t add)
{
	return uc_vector_extend_init_(base, capacity, itemsize, UC_VECTOR_INIT_SIZE, count, add);
}

#define uc_vector_reduce_init(vec, init, remove) \
	uc_vector_reduce_init_((char **)&(vec)->entries, &(vec)->capacity, \
		sizeof((vec)->entries[0]), (init), (vec)->count, (remove))

#define uc_vector_extend_init(vec, init, add) \
	(__typeof__((vec)->entries + 0)) uc_vector_extend_init_( \
		(char **)&(vec)->entries, &(vec)->capacity, \
		sizeof((vec)->entries[0]), \
		(init), (vec)->count, (add))

#define uc_vector_reduce(vec, remove) \
	uc_vector_reduce_init((vec), UC_VECTOR_INIT_SIZE, (remove))

#define uc_vector_extend(vec, add) \
	uc_vector_extend_init((vec), UC_VECTOR_INIT_SIZE, (add))

#define uc_vector_grow(vec) \
	uc_vector_extend((vec), 1)

#define uc_vector_clear(vec) \
	do { \
		free((vec)->entries); \
		(vec)->entries = NULL; \
		(vec)->count = 0; \
		(vec)->capacity = 0; \
	} while(0)

#define uc_vector_first(vec) \
	(&((vec)->entries[0]))

#define uc_vector_last(vec) \
	((vec)->count ? &((vec)->entries[(vec)->count - 1]) : NULL)

#define uc_vector_push(vec, ...) ({ \
	*uc_vector_extend((vec), 1) = ((__typeof__((vec)->entries[0]))__VA_ARGS__); \
	&(vec)->entries[(vec)->count++]; \
})

#define uc_vector_pop(vec) \
	((vec)->count ? &(vec)->entries[--(vec)->count] : NULL)

#define uc_vector_foreach(vec, iter) \
	for (__typeof__((vec)->entries + 0) iter = (vec)->entries; \
	     iter < (vec)->entries + (vec)->count; \
	     iter++)

#define uc_vector_foreach_reverse(vec, iter) \
	for (__typeof__((vec)->entries + 0) iter = (vec)->count \
	         ? (vec)->entries + (vec)->count - 1 : NULL; \
	     iter != NULL && iter >= (vec)->entries; \
	     iter--)

#ifdef __APPLE__

# define uc_vector_sort_cb(fn_, optype_, udtype_, ...) \
	int fn_(void *ud, const void *k1, const void *k2) { \
		optype_ v1 = *(optype_ *)k1; \
		optype_ v2 = *(optype_ *)k2; \
		udtype_ ctx = (udtype_)ud; \
		__VA_ARGS__ \
	}

# define uc_vector_sort(v_, cmp_, ud_) \
	qsort_r((v_)->entries, (v_)->count, sizeof((v_)->entries[0]), ud_, cmp_)

#else

# define uc_vector_sort_cb(fn_, optype_, udtype_, ...) \
	int fn_(const void *k1, const void *k2, void *ud) { \
		optype_ v1 = *(optype_ *)k1; \
		optype_ v2 = *(optype_ *)k2; \
		udtype_ ctx = (udtype_)ud; \
		__VA_ARGS__ \
	}

# define uc_vector_sort(v_, cmp_, ud_) \
	qsort_r((v_)->entries, (v_)->count, sizeof((v_)->entries[0]), cmp_, ud_)

#endif

#endif /* UCODE_UTIL_H */
