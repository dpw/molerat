#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <molerat/buffer.h>

void drainbuf_advance(struct drainbuf *drainbuf, size_t step)
{
	assert(step <= drainbuf_length(drainbuf));
	drainbuf->pos += step;
}

void growbuf_init(struct growbuf *growbuf, size_t capacity)
{
	growbuf->end = growbuf->start = xalloc(capacity);
	growbuf->limit = growbuf->start + capacity;
	growbuf->frozen = FALSE;
}

void growbuf_fini(struct growbuf *growbuf)
{
	free(growbuf->start);
}

void growbuf_reset(struct growbuf *growbuf)
{
	growbuf->end = growbuf->start;
	growbuf->frozen = FALSE;
}

void growbuf_to_drainbuf(struct growbuf *growbuf, struct drainbuf *drainbuf)
{
	/* Mark that the growbuf can no longer be grown */
	growbuf->frozen = TRUE;
	drainbuf->pos = growbuf->start;
	drainbuf->end = growbuf->end;
}

void growbuf_shift(struct growbuf *growbuf, size_t pos)
{
	assert(!growbuf_frozen(growbuf));
	if (!pos)
		return;

	assert(pos <= growbuf_length(growbuf));
	memmove(growbuf->start, growbuf->start + pos,
		growbuf_length(growbuf) - pos);
	growbuf->end -= pos;
}

void *growbuf_reserve(struct growbuf *growbuf, size_t need)
{
	assert(!growbuf_frozen(growbuf));
	assert(growbuf->limit - growbuf->end >= 0);

	if (need > (size_t)(growbuf->limit - growbuf->end)) {
		size_t capacity = growbuf->limit - growbuf->start;
		size_t used = growbuf->end - growbuf->start;

		do
			capacity *= 2;
		while (capacity - used < need);

		growbuf->start = xrealloc(growbuf->start, capacity);
		growbuf->limit = growbuf->start + capacity;
		growbuf->end = growbuf->start + used;
	}

	return growbuf->end;
}

void growbuf_append(struct growbuf *growbuf, const void *data, size_t len)
{
	memcpy(growbuf_reserve(growbuf, len), data, len);
	growbuf_advance(growbuf, len);
}

void growbuf_append_string(struct growbuf *growbuf, const char *s)
{
	growbuf_append(growbuf, s, strlen(s));
}

static void grow(struct growbuf *growbuf)
{
	size_t used = growbuf->end - growbuf->start;
	size_t capacity = (growbuf->limit - growbuf->start) * 2;
	growbuf->start = xrealloc(growbuf->start, capacity);
	growbuf->limit = growbuf->start + capacity;
	growbuf->end = growbuf->start + used;
}

void growbuf_vprintf(struct growbuf *growbuf, const char *fmt, va_list ap)
{
	assert(!growbuf_frozen(growbuf));

	for (;;) {
		long space = growbuf->limit - growbuf->end;
		va_list ap2;
		int res;

		va_copy(ap2, ap);
		res = vsnprintf(growbuf->end, space, fmt, ap2);
		va_end(ap2);

		if (res < 0)
			die("vsnprintf failed");

		/* vsnprintf returns the number of characters it would
		   have written.  That excludes the trailing null,
		   which we don't care about. */
		if (res <= space) {
			growbuf->end += res;
			break;
		}

		grow(growbuf);
	}
}

void growbuf_printf(struct growbuf *growbuf, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	growbuf_vprintf(growbuf, fmt, ap);
	va_end(ap);
}
