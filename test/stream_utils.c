#include <string.h>

#include <molerat/tasklet.h>

#include "stream_utils.h"

struct bytes_read_stream {
	struct stream base;
	struct bytes buf;
};

static struct stream_ops bytes_read_stream_ops;

struct stream *bytes_read_stream_create(struct bytes buf)
{
	struct bytes_read_stream *s = xalloc(sizeof *s);
	s->base.ops = &bytes_read_stream_ops;
	s->buf = buf;
	return &s->base;
}

static ssize_t bytes_read_stream_read(struct stream *gs, void *buf,
					 size_t len, struct tasklet *t,
					 struct error *err)
{
	struct bytes_read_stream *s
		= container_of(gs, struct bytes_read_stream, base);
	size_t buf_len = bytes_length(s->buf);

	(void)t;
	(void)err;

	if (buf_len == 0)
		return STREAM_END;

	if (len > buf_len)
		len = buf_len;

	memcpy(buf, bytes_current(s->buf), len);
	bytes_advance(&s->buf, len);
	return len;
}

static void bytes_read_stream_destroy(struct stream *gs)
{
	struct bytes_read_stream *s
		= container_of(gs, struct bytes_read_stream, base);
	free(s);
}

static struct stream_ops bytes_read_stream_ops = {
	bytes_read_stream_destroy,
	bytes_read_stream_read,
	stream_read_only_write,
	stream_noop_close
};


struct growbuf_write_stream {
	struct stream base;
	struct growbuf *growbuf;
};

static struct stream_ops growbuf_write_stream_ops;

struct stream *growbuf_write_stream_create(struct growbuf *growbuf)
{
	struct growbuf_write_stream *s = xalloc(sizeof *s);
	s->base.ops = &growbuf_write_stream_ops;
	s->growbuf = growbuf;
	return &s->base;
}

static ssize_t growbuf_write_stream_write(struct stream *gs, const void *buf,
					  size_t len, struct tasklet *t,
					  struct error *err)
{
	struct growbuf_write_stream *s
		= container_of(gs, struct growbuf_write_stream, base);

	(void)t;
	(void)err;

	growbuf_append(s->growbuf, buf, len);
	return len;
}

static void growbuf_write_stream_destroy(struct stream *gs)
{
	struct growbuf_write_stream *s
		= container_of(gs, struct growbuf_write_stream, base);
	free(s);
}

static struct stream_ops growbuf_write_stream_ops = {
	growbuf_write_stream_destroy,
	stream_write_only_read,
	growbuf_write_stream_write,
	stream_noop_close
};


struct byte_at_a_time_stream {
	struct stream base;
	struct stream *underlying;
	bool_t waited;
};

static struct stream_ops byte_at_a_time_stream_ops;

struct stream *byte_at_a_time_stream_create(struct stream *underlying)
{
	struct byte_at_a_time_stream *s = xalloc(sizeof *s);
	s->base.ops = &byte_at_a_time_stream_ops;
	s->underlying = underlying;
	s->waited = FALSE;
	return &s->base;
}

static ssize_t byte_at_a_time_stream_read(struct stream *gs, void *buf,
					  size_t len, struct tasklet *t,
					  struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);

	if (s->waited) {
		s->waited = FALSE;
		return stream_read(s->underlying, buf, len > 0, t, err);
	}
	else {
		s->waited = TRUE;
		tasklet_run(t);
		return STREAM_WAITING;
	}
}

static ssize_t byte_at_a_time_stream_write(struct stream *gs, const void *buf,
					   size_t len, struct tasklet *t,
					   struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);

	if (s->waited) {
		s->waited = FALSE;
		return stream_write(s->underlying, buf, len > 0, t, err);
	}
	else {
		s->waited = TRUE;
		tasklet_run(t);
		return STREAM_WAITING;
	}
}

static void byte_at_a_time_stream_destroy(struct stream *gs)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	stream_destroy(s->underlying);
	free(s);
}

static enum stream_result byte_at_a_time_stream_close(struct stream *gs,
						      struct tasklet *t,
						      struct error *err)
{
	struct byte_at_a_time_stream *s
		= container_of(gs, struct byte_at_a_time_stream, base);
	return stream_close(s->underlying, t, err);
}

static struct stream_ops byte_at_a_time_stream_ops = {
	byte_at_a_time_stream_destroy,
	byte_at_a_time_stream_read,
	byte_at_a_time_stream_write,
	byte_at_a_time_stream_close
};
