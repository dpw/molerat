#include <string.h>

#include <molerat/buffer.h>

#include "stream_utils.h"

struct drainbuf_read_stream {
	struct stream base;
	struct drainbuf buf;
};

static struct stream_ops drainbuf_read_stream_ops;

struct stream *drainbuf_read_stream_create(struct drainbuf *buf)
{
	struct drainbuf_read_stream *s = xalloc(sizeof *s);
	s->base.ops = &drainbuf_read_stream_ops;
	s->buf = *buf;
	return &s->base;
}

ssize_t drainbuf_read_stream_read(struct stream *gs, void *buf, size_t len,
			      struct tasklet *t, struct error *err)
{
	struct drainbuf_read_stream *s
		= container_of(gs, struct drainbuf_read_stream, base);
	size_t buf_len = drainbuf_length(&s->buf);

	(void)t;
	(void)err;

	if (buf_len == 0)
		return STREAM_END;

	if (len > buf_len)
		len = buf_len;

	memcpy(buf, drainbuf_current(&s->buf), len);
	drainbuf_advance(&s->buf, len);
	return len;
}

ssize_t drainbuf_read_stream_write(struct stream *gs, const void *buf,
			      size_t len, struct tasklet *t, struct error *err)
{
	(void)gs;
	(void)buf;
	(void)len;
	(void)t;

	error_set(err, ERROR_INVALID, "cannot write to a drainbuf_read_stream");
	return STREAM_ERROR;
}

static void drainbuf_read_stream_destroy(struct stream *gs)
{
	struct drainbuf_read_stream *s
		= container_of(gs, struct drainbuf_read_stream, base);
	free(s);
}

static bool_t drainbuf_read_stream_close(struct stream *gs, struct error *err)
{
	(void)gs;
	(void)err;

	return TRUE;
}

static struct stream_ops drainbuf_read_stream_ops = {
	drainbuf_read_stream_destroy,
	drainbuf_read_stream_read,
	drainbuf_read_stream_write,
	drainbuf_read_stream_close
};
