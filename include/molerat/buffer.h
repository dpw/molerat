#ifndef MOLERAT_BUFFER_H
#define MOLERAT_BUFFER_H

#include <stdarg.h>
#include <assert.h>

#include <molerat/base.h>

/* drainbufs don't own the underlying buffer */
struct drainbuf {
	const char *pos;
	const char *end;
};

static inline void drainbuf_set(struct drainbuf *drainbuf, const char *p,
				size_t len)
{
	drainbuf->pos = p;
	drainbuf->end = drainbuf->pos + len;
}

static inline size_t drainbuf_length(struct drainbuf *drainbuf)
{
	return drainbuf->end - drainbuf->pos;
}

static inline const char *drainbuf_current(struct drainbuf *drainbuf)
{
	return drainbuf->pos;
}

void drainbuf_advance(struct drainbuf *drainbuf, size_t step);

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
void growbuf_to_drainbuf(struct growbuf *growbuf, struct drainbuf *drainbuf);
void *growbuf_grow(struct growbuf *growbuf, size_t need);
void growbuf_shift(struct growbuf *growbuf, size_t pos);
void growbuf_append(struct growbuf *growbuf, const void *data, size_t len);
void growbuf_append_string(struct growbuf *growbuf, const char *s);
void growbuf_printf(struct growbuf *growbuf, const char *fmt, ...);
void growbuf_vprintf(struct growbuf *growbuf, const char *fmt, va_list ap);

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
