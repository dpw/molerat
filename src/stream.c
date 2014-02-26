#include <molerat/stream.h>
#include <molerat/buffer.h>

ssize_t stream_read_only_write(struct stream *gs, const void *buf,
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

ssize_t stream_write_only_read(struct stream *gs, void *buf,
			       size_t len, struct tasklet *t,
			       struct error *err)
{
	(void)gs;
	(void)buf;
	(void)len;
	(void)t;

	error_set(err, ERROR_INVALID, "read from a write-only stream");
	return STREAM_ERROR;
}

enum stream_result stream_noop_close(struct stream *gs, struct tasklet *t,
				     struct error *err)
{
	(void)gs;
	(void)t;
	(void)err;

	return STREAM_OK;
}

ssize_t stream_read_growbuf(struct stream *s, struct growbuf *gb,
			    struct tasklet *t, struct error *err)
{
	ssize_t res;
	size_t space = growbuf_space(gb);

	if (!space) {
		/* Growbuf is full, so make some space */
		growbuf_grow(gb, 1);
		space = growbuf_space(gb);
	}

	res = stream_read(s, growbuf_end(gb), space, t, err);
	if (res > 0)
		growbuf_advance(gb, res);

	return res;
}

ssize_t stream_write_bytes(struct stream *s, struct bytes *b,
			   struct tasklet *t, struct error *err)
{
	size_t l = bytes_length(*b);
	ssize_t res;

	if (!l)
		return STREAM_END;

	res = stream_write(s, bytes_current(*b), bytes_length(*b), t, err);
	if (res > 0)
		bytes_advance(b, res);

	return res;
}
