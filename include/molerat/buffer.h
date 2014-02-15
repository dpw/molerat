#ifndef MOLERAT_BUFFER_H
#define MOLERAT_BUFFER_H

#include <stdarg.h>
#include <assert.h>

#include <molerat/base.h>

/* bytess don't own the underlying buffer */
struct bytes {
	const char *pos;
	const char *end;
};

static inline struct bytes make_bytes(const char *p, size_t len)
{
	struct bytes b;
	b.pos = p;
	b.end = p + len;
	return b;
}

static inline size_t bytes_length(struct bytes bytes)
{
	return bytes.end - bytes.pos;
}

static inline const char *bytes_current(struct bytes bytes)
{
	return bytes.pos;
}

void bytes_advance(struct bytes *bytes, size_t step);
struct bytes c_string_bytes(const char *str);
int bytes_compare(struct bytes a, struct bytes b);

/* growbufs do own the underlying buffer */
struct growbuf {
	char *start;
	char *limit;
	char *end;
	bool_t frozen;
};

void growbuf_init(struct growbuf *growbuf, size_t capacity);
void growbuf_fini(struct growbuf *growbuf);
void growbuf_reset(struct growbuf *growbuf);
void *growbuf_grow(struct growbuf *growbuf, size_t need);
void growbuf_shift(struct growbuf *growbuf, size_t pos);
void growbuf_append(struct growbuf *growbuf, const void *data, size_t len);
void growbuf_append_string(struct growbuf *growbuf, const char *s);
void growbuf_printf(struct growbuf *growbuf, const char *fmt, ...);
void growbuf_vprintf(struct growbuf *growbuf, const char *fmt, va_list ap);

static inline struct bytes growbuf_to_bytes(struct growbuf *growbuf)
{
	struct bytes res;

	/* Mark that the growbuf can no longer be grown */
	growbuf->frozen = TRUE;
	res.pos = growbuf->start;
	res.end = growbuf->end;
	return res;
}

static inline bool_t growbuf_frozen(const struct growbuf *growbuf)
{
	return growbuf->frozen;
}

static inline size_t growbuf_length(const struct growbuf *growbuf)
{
	return growbuf->end - growbuf->start;
}

static inline void *growbuf_offset(const struct growbuf *growbuf, size_t offset)
{
	return growbuf->start + offset;
}

static inline size_t growbuf_offset_of(const struct growbuf *growbuf, void *p)
{
	return (char *)p - growbuf->start;
}

static inline size_t growbuf_space(const struct growbuf *growbuf)
{
	assert(!growbuf_frozen(growbuf));
	return growbuf->limit - growbuf->end;
}

static inline void *growbuf_end(const struct growbuf *growbuf)
{
	return growbuf->end;
}

static inline void *growbuf_reserve(struct growbuf *growbuf, size_t need)
{
	assert(!growbuf_frozen(growbuf));

	if (need <= (size_t)(growbuf->limit - growbuf->end))
		return growbuf->end;
	else
		return growbuf_grow(growbuf, need);
}

static inline void growbuf_advance(struct growbuf *growbuf, size_t len)
{
	growbuf->end += len;
	assert(growbuf->end <= growbuf->limit);
}

static inline void growbuf_append_growbuf(struct growbuf *dest,
					  const struct growbuf *src)
{
	growbuf_append(dest, growbuf_offset(src, 0), growbuf_length(src));
}


#endif
