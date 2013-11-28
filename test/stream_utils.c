#include <string.h>

#include <molerat/buffer.h>

#include "stream_utils.h"

static ssize_t read_only_stream_write(struct stream *gs, const void *buf,
				      size_t len, struct tasklet *t,
				      struct error *err)
{
	(void)gs;
	(void)buf;
	(void)len;
	(void)t;

	error_set(err, ERROR_INVALID, "write to a read-only stream");
	return STREAM_ERROR;
}

struct bytes_read_stream {
	struct stream base;
	struct bytes buf;
};

static struct stream_ops bytes_read_stream_ops;

struct stream *bytes_read_stream_create(struct bytes *buf)
{
	struct bytes_read_stream *s = xalloc(sizeof *s);
	s->base.ops = &bytes_read_stream_ops;
	s->buf = *buf;
	return &s->base;
}

static ssize_t bytes_read_stream_read(struct stream *gs, void *buf,
					 size_t len, struct tasklet *t,
					 struct error *err)
{
	struct bytes_read_stream *s
		= container_of(gs, struct bytes_read_stream, base);
	size_t buf_len = bytes_length(&s->buf);

	(void)t;
	(void)err;

	if (buf_len == 0)
		return STREAM_END;

	if (len > buf_len)
		len = buf_len;

	memcpy(buf, bytes_current(&s->buf), len);
	bytes_advance(&s->buf, len);
	return len;
}

static void bytes_read_stream_destroy(struct stream *gs)
{
	struct bytes_read_stream *s
		= container_of(gs, struct bytes_read_stream, base);
	free(s);
}

static bool_t bytes_read_stream_close(struct stream *gs, struct error *err)
{
	(void)gs;
	(void)err;

	return TRUE;
}

static struct stream_ops bytes_read_stream_ops = {
	bytes_read_stream_destroy,
	bytes_read_stream_read,
	read_only_stream_write,
	bytes_read_stream_close
};


struct byte_at_a_time_stream {
	struct stream base;
	struct stream *underlying;
};

static struct stream_ops byte_at_a_time_stream_ops;

struct stream *byte_at_a_time_stream_create(struct stream *underlying)
{
	struct byte_at_a_time_stream *s = xalloc(sizeof *s);
	s->base.ops = &byte_at_a_time_stream_ops;
	s->underlying = underlying;
	return &s->base;
}

static ssize_t byte_at_a_time_stream_read(struct stream *gs, void *buf,
					       size_t len, struct tasklet *t,
					       struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	return stream_read(s->underlying, buf, len > 0, t, err);
}

static ssize_t byte_at_a_time_stream_write(struct stream *gs, const void *buf,
					   size_t len, struct tasklet *t,
					   struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	return stream_write(s->underlying, buf, len > 0, t, err);
}

static void byte_at_a_time_stream_destroy(struct stream *gs)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	stream_destroy(s->underlying);
	free(s);
}

static bool_t byte_at_a_time_stream_close(struct stream *gs,
					       struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	return stream_close(s->underlying, err);
}

static struct stream_ops byte_at_a_time_stream_ops = {
	byte_at_a_time_stream_destroy,
	byte_at_a_time_stream_read,
	byte_at_a_time_stream_write,
	byte_at_a_time_stream_close
};
